// SPDX-License-Identifier: MPL-2.0

package main

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"testing"
)

func TestEncodeICOContainsEveryPNGSize(t *testing.T) {
	source := image.NewNRGBA(image.Rect(0, 0, 64, 64))
	for y := 0; y < 64; y++ {
		for x := 0; x < 64; x++ {
			source.SetNRGBA(x, y, color.NRGBA{R: byte(x * 4), G: byte(y * 4), B: 0x80, A: byte(128 + x)})
		}
	}
	sizes := []int{16, 32, 256}
	var encoded bytes.Buffer
	if err := encodeICO(&encoded, source, sizes); err != nil {
		t.Fatal(err)
	}
	data := encoded.Bytes()
	if len(data) < 6 || binary.LittleEndian.Uint16(data[0:2]) != 0 || binary.LittleEndian.Uint16(data[2:4]) != 1 {
		t.Fatal("invalid ICO header")
	}
	if count := int(binary.LittleEndian.Uint16(data[4:6])); count != len(sizes) {
		t.Fatalf("ICO contains %d images, want %d", count, len(sizes))
	}
	for index, expected := range sizes {
		entry := data[6+16*index : 6+16*(index+1)]
		storedWidth := int(entry[0])
		if storedWidth == 0 {
			storedWidth = 256
		}
		if storedWidth != expected {
			t.Fatalf("entry %d width is %d, want %d", index, storedWidth, expected)
		}
		length := int(binary.LittleEndian.Uint32(entry[8:12]))
		offset := int(binary.LittleEndian.Uint32(entry[12:16]))
		if offset < 0 || length <= 0 || offset+length > len(data) {
			t.Fatalf("entry %d has invalid payload range %d:%d", index, offset, offset+length)
		}
		frame, err := png.Decode(bytes.NewReader(data[offset : offset+length]))
		if err != nil {
			t.Fatalf("entry %d is not a PNG: %v", index, err)
		}
		if frame.Bounds().Dx() != expected || frame.Bounds().Dy() != expected {
			t.Fatalf("entry %d decoded to %v, want %dx%d", index, frame.Bounds(), expected, expected)
		}
		if frame.ColorModel() != color.NRGBAModel {
			t.Fatalf("entry %d is not 8-bit RGBA: %T (%v)", index, frame, frame.ColorModel())
		}
	}
}

func TestResizeBilinearPreservesTransparentColor(t *testing.T) {
	source := image.NewNRGBA(image.Rect(0, 0, 2, 2))
	source.SetNRGBA(0, 0, color.NRGBA{R: 255, A: 255})
	source.SetNRGBA(1, 0, color.NRGBA{G: 255, A: 255})
	source.SetNRGBA(0, 1, color.NRGBA{B: 255, A: 0})
	source.SetNRGBA(1, 1, color.NRGBA{R: 255, G: 255, B: 255, A: 255})
	resized := resizeBilinear(source, 4)
	if resized.Bounds() != image.Rect(0, 0, 4, 4) {
		t.Fatalf("unexpected resized bounds: %v", resized.Bounds())
	}
	for _, point := range []image.Point{{0, 0}, {2, 2}, {3, 3}} {
		_, _, _, alpha := resized.At(point.X, point.Y).RGBA()
		if alpha > 0xffff {
			t.Fatalf("invalid alpha at %v: %d", point, alpha)
		}
	}
}
