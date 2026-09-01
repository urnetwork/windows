// Command make-icons generates the Windows tray and application ICO files from
// the macOS asset catalog. The outputs are committed and should be regenerated
// whenever the shared brand art changes.
//
// Usage from windows/app:
//
//	go run ./tools/make-icons
//
// SPDX-License-Identifier: MPL-2.0
package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

var (
	states = []string{"NoProvideNoConnect", "NoProvideConnect", "ProvideNoConnect", "ProvideConnect"}
	snake  = map[string]string{
		"NoProvideNoConnect": "noprovide_noconnect",
		"NoProvideConnect":   "noprovide_connect",
		"ProvideNoConnect":   "provide_noconnect",
		"ProvideConnect":     "provide_connect",
	}
	traySizes = []int{16, 20, 24, 32, 48}
	appSizes  = []int{16, 24, 32, 48, 64, 128, 256}
)

type paths struct {
	appleAssets string
	output      string
}

func defaultPaths() (paths, error) {
	_, filename, _, ok := runtime.Caller(0)
	if !ok {
		return paths{}, errors.New("cannot locate make-icons source")
	}
	windowsRoot := filepath.Clean(filepath.Join(filepath.Dir(filename), "..", "..", ".."))
	workspaceRoot := filepath.Dir(windowsRoot)
	return paths{
		appleAssets: filepath.Join(workspaceRoot, "apple", "app", "network", "Assets.xcassets"),
		output:      filepath.Join(windowsRoot, "app", "src", "App", "Assets"),
	}, nil
}

func menuBarSource(appleAssets, mode, state string) string {
	directory := filepath.Join(appleAssets, "Icons", "MenuBar"+state+".imageset")
	return filepath.Join(directory, "MenuBar"+mode+state+"32.png")
}

func appSource(appleAssets string) (string, error) {
	directory := filepath.Join(appleAssets, "AppIcon.appiconset")
	entries, err := os.ReadDir(directory)
	if err != nil {
		return "", err
	}
	var largest string
	var largestSize int64 = -1
	for _, entry := range entries {
		if entry.IsDir() || !strings.EqualFold(filepath.Ext(entry.Name()), ".png") {
			continue
		}
		filename := filepath.Join(directory, entry.Name())
		if strings.Contains(entry.Name(), "1024") {
			return filename, nil
		}
		info, err := entry.Info()
		if err != nil {
			return "", err
		}
		if info.Size() > largestSize {
			largest = filename
			largestSize = info.Size()
		}
	}
	if largest == "" {
		return "", fmt.Errorf("no PNG app icon found under %s", directory)
	}
	return largest, nil
}

func clamp(value, low, high float64) float64 {
	if value < low {
		return low
	}
	if value > high {
		return high
	}
	return value
}

func resizeBilinear(source image.Image, size int) *image.NRGBA {
	bounds := source.Bounds()
	width := bounds.Dx()
	height := bounds.Dy()
	destination := image.NewNRGBA(image.Rect(0, 0, size, size))
	if width == 0 || height == 0 || size <= 0 {
		return destination
	}

	for y := 0; y < size; y++ {
		sourceY := (float64(y)+0.5)*float64(height)/float64(size) - 0.5
		y0 := int(math.Floor(sourceY))
		fy := sourceY - float64(y0)
		if y0 < 0 {
			y0, fy = 0, 0
		}
		y1 := y0 + 1
		if y1 >= height {
			y1 = height - 1
		}
		for x := 0; x < size; x++ {
			sourceX := (float64(x)+0.5)*float64(width)/float64(size) - 0.5
			x0 := int(math.Floor(sourceX))
			fx := sourceX - float64(x0)
			if x0 < 0 {
				x0, fx = 0, 0
			}
			x1 := x0 + 1
			if x1 >= width {
				x1 = width - 1
			}

			type rgba struct{ r, g, b, a float64 }
			sample := func(px, py int) rgba {
				r, g, b, a := source.At(bounds.Min.X+px, bounds.Min.Y+py).RGBA()
				return rgba{float64(r), float64(g), float64(b), float64(a)}
			}
			topLeft := sample(x0, y0)
			topRight := sample(x1, y0)
			bottomLeft := sample(x0, y1)
			bottomRight := sample(x1, y1)
			interpolate := func(a, b, c, d float64) float64 {
				top := a + (b-a)*fx
				bottom := c + (d-c)*fx
				return top + (bottom-top)*fy
			}
			alpha := interpolate(topLeft.a, topRight.a, bottomLeft.a, bottomRight.a)
			red := interpolate(topLeft.r, topRight.r, bottomLeft.r, bottomRight.r)
			green := interpolate(topLeft.g, topRight.g, bottomLeft.g, bottomRight.g)
			blue := interpolate(topLeft.b, topRight.b, bottomLeft.b, bottomRight.b)
			straight := func(premultiplied float64) uint8 {
				if alpha <= 0 {
					return 0
				}
				return uint8(math.Round(clamp(premultiplied*255/alpha, 0, 255)))
			}
			destination.SetNRGBA(x, y, color.NRGBA{
				R: straight(red), G: straight(green), B: straight(blue),
				A: uint8(math.Round(clamp(alpha*255/65535, 0, 255))),
			})
		}
	}
	return destination
}

func encodeICO(writer io.Writer, source image.Image, sizes []int) error {
	if len(sizes) == 0 || len(sizes) > math.MaxUint16 {
		return fmt.Errorf("invalid ICO image count: %d", len(sizes))
	}
	frames := make([][]byte, 0, len(sizes))
	for _, size := range sizes {
		if size <= 0 || size > 256 {
			return fmt.Errorf("unsupported ICO size: %d", size)
		}
		var encoded bytes.Buffer
		if err := png.Encode(&encoded, resizeBilinear(source, size)); err != nil {
			return err
		}
		frames = append(frames, encoded.Bytes())
	}

	var output bytes.Buffer
	for _, value := range []uint16{0, 1, uint16(len(frames))} {
		if err := binary.Write(&output, binary.LittleEndian, value); err != nil {
			return err
		}
	}
	offset := uint32(6 + 16*len(frames))
	for index, frame := range frames {
		sizeByte := byte(sizes[index])
		if sizes[index] == 256 {
			sizeByte = 0
		}
		entry := []byte{sizeByte, sizeByte, 0, 0}
		output.Write(entry)
		for _, value := range []uint16{1, 32} {
			if err := binary.Write(&output, binary.LittleEndian, value); err != nil {
				return err
			}
		}
		if err := binary.Write(&output, binary.LittleEndian, uint32(len(frame))); err != nil {
			return err
		}
		if err := binary.Write(&output, binary.LittleEndian, offset); err != nil {
			return err
		}
		offset += uint32(len(frame))
	}
	for _, frame := range frames {
		output.Write(frame)
	}
	_, err := writer.Write(output.Bytes())
	return err
}

func writeICO(sourcePNG, destination string, sizes []int) error {
	file, err := os.Open(sourcePNG)
	if err != nil {
		return err
	}
	source, err := png.Decode(file)
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	if err := os.MkdirAll(filepath.Dir(destination), 0o755); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(filepath.Dir(destination), ".make-icons-*.ico")
	if err != nil {
		return err
	}
	temporaryName := temporary.Name()
	defer os.Remove(temporaryName)
	if err := encodeICO(temporary, source, sizes); err != nil {
		temporary.Close()
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	if err := os.Chmod(temporaryName, 0o644); err != nil {
		return err
	}
	return os.Rename(temporaryName, destination)
}

func generate(config paths) error {
	fmt.Println("tray icons:")
	for _, state := range states {
		for _, mapping := range []struct {
			mode   string
			prefix string
		}{{mode: "Light", prefix: "tray_light_"}, {mode: "Dark", prefix: "tray_dark_"}} {
			source := menuBarSource(config.appleAssets, mapping.mode, state)
			name := mapping.prefix + snake[state] + ".ico"
			if err := writeICO(source, filepath.Join(config.output, name), traySizes); err != nil {
				return fmt.Errorf("generate %s: %w", name, err)
			}
			relative, _ := filepath.Rel(config.appleAssets, source)
			fmt.Printf("  %s  <- %s\n", name, relative)
		}
	}
	fmt.Println("app icon:")
	source, err := appSource(config.appleAssets)
	if err != nil {
		return err
	}
	if err := writeICO(source, filepath.Join(config.output, "app.ico"), appSizes); err != nil {
		return fmt.Errorf("generate app.ico: %w", err)
	}
	relative, _ := filepath.Rel(config.appleAssets, source)
	fmt.Printf("  app.ico  <- %s\n", relative)
	fmt.Println("done.")
	return nil
}

func run(arguments []string) error {
	defaults, err := defaultPaths()
	if err != nil {
		return err
	}
	flags := flag.NewFlagSet("make-icons", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	appleAssets := flags.String("apple-assets", defaults.appleAssets, "macOS Assets.xcassets path")
	output := flags.String("out", defaults.output, "Windows Assets output path")
	if err := flags.Parse(arguments); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected argument: %s", flags.Arg(0))
	}
	return generate(paths{appleAssets: *appleAssets, output: *output})
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
