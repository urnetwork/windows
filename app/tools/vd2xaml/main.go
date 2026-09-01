// Command vd2xaml converts an Android VectorDrawable into a WinUI 3 XAML
// fragment.
//
// The brand icons live in the Android repository as VectorDrawables. Their
// pathData is already SVG path syntax, which WinUI's Path.Data accepts
// unchanged, so conversion wraps the paths in a viewport-sized Canvas and
// translates inline Android gradients to absolute WinUI gradient brushes.
//
// Usage:
//
//	go run ./tools/vd2xaml <input.xml> [--name Foo] [--size 18]
//
// SPDX-License-Identifier: MPL-2.0
package main

import (
	"encoding/xml"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"unicode"
)

const (
	androidNamespace = "http://schemas.android.com/apk/res/android"
	aaptNamespace    = "http://schemas.android.com/aapt"
)

type element struct {
	XMLName  xml.Name
	Attrs    []xml.Attr `xml:",any,attr"`
	Children []element  `xml:",any"`
}

type options struct {
	input string
	name  string
	size  *float64
}

func (node *element) attribute(namespace, local string) (string, bool) {
	for _, attribute := range node.Attrs {
		if attribute.Name.Space == namespace && attribute.Name.Local == local {
			return attribute.Value, true
		}
	}
	return "", false
}

func (node *element) children(namespace, local string) []*element {
	var result []*element
	for index := range node.Children {
		child := &node.Children[index]
		if child.XMLName.Space == namespace && child.XMLName.Local == local {
			result = append(result, child)
		}
	}
	return result
}

func parseDP(value string, fallback float64) (float64, error) {
	if value == "" {
		return fallback, nil
	}
	trimmed := strings.TrimRightFunc(value, unicode.IsLetter)
	parsed, err := strconv.ParseFloat(trimmed, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid dimension %q: %w", value, err)
	}
	return parsed, nil
}

func floatString(value float64) string {
	result := strconv.FormatFloat(value, 'f', -1, 64)
	if !strings.Contains(result, ".") {
		result += ".0"
	}
	return result
}

func xamlColor(value string) string {
	color := strings.TrimSpace(value)
	if !strings.HasPrefix(color, "#") {
		return ""
	}
	if len(color) == 4 {
		return fmt.Sprintf("#%c%c%c%c%c%c", color[1], color[1], color[2], color[2], color[3], color[3])
	}
	return color
}

func escapeAttribute(value string) string {
	var output strings.Builder
	_ = xml.EscapeText(&output, []byte(value))
	return output.String()
}

func gradientXAML(gradient *element, indent int) (string, error) {
	gradientType, _ := gradient.attribute(androidNamespace, "type")
	if gradientType == "" {
		gradientType = "linear"
	}
	if gradientType != "linear" {
		return "", fmt.Errorf("unsupported gradient type: %s", gradientType)
	}
	coordinate := func(name string) (float64, error) {
		value, _ := gradient.attribute(androidNamespace, name)
		return parseDP(value, 0)
	}
	x1, err := coordinate("startX")
	if err != nil {
		return "", err
	}
	y1, err := coordinate("startY")
	if err != nil {
		return "", err
	}
	x2, err := coordinate("endX")
	if err != nil {
		return "", err
	}
	y2, err := coordinate("endY")
	if err != nil {
		return "", err
	}

	type stop struct {
		offset float64
		color  string
	}
	var stops []stop
	for _, item := range gradient.children("", "item") {
		offsetValue, _ := item.attribute(androidNamespace, "offset")
		offset, err := parseDP(offsetValue, 0)
		if err != nil {
			return "", err
		}
		colorValue, _ := item.attribute(androidNamespace, "color")
		stops = append(stops, stop{offset: offset, color: xamlColor(colorValue)})
	}
	if len(stops) == 0 {
		start, _ := gradient.attribute(androidNamespace, "startColor")
		end, _ := gradient.attribute(androidNamespace, "endColor")
		stops = []stop{{offset: 0, color: xamlColor(start)}, {offset: 1, color: xamlColor(end)}}
	}

	padding := strings.Repeat(" ", indent)
	var output strings.Builder
	fmt.Fprintf(&output, `%s<LinearGradientBrush MappingMode="Absolute" StartPoint="%s,%s" EndPoint="%s,%s">`,
		padding, floatString(x1), floatString(y1), floatString(x2), floatString(y2))
	for _, item := range stops {
		fmt.Fprintf(&output, "\n%s  <GradientStop Offset=\"%s\" Color=\"%s\" />",
			padding, floatString(item.offset), escapeAttribute(item.color))
	}
	fmt.Fprintf(&output, "\n%s</LinearGradientBrush>", padding)
	return output.String(), nil
}

func convert(filename, name string, size *float64) (string, error) {
	data, err := os.ReadFile(filename)
	if err != nil {
		return "", err
	}
	var root element
	if err := xml.Unmarshal(data, &root); err != nil {
		return "", err
	}
	viewportWidth, _ := root.attribute(androidNamespace, "viewportWidth")
	viewportHeight, _ := root.attribute(androidNamespace, "viewportHeight")
	width, err := parseDP(viewportWidth, 24)
	if err != nil {
		return "", err
	}
	height, err := parseDP(viewportHeight, 24)
	if err != nil {
		return "", err
	}

	var body []string
	// path, gradient, and item elements are unqualified. Only their Android
	// attributes are namespaced; qualifying the elements silently loses paths.
	for _, path := range root.children("", "path") {
		pathData, _ := path.attribute(androidNamespace, "pathData")
		if pathData == "" {
			continue
		}
		pathData = strings.Join(strings.Fields(pathData), " ")
		fillValue, _ := path.attribute(androidNamespace, "fillColor")
		fill := xamlColor(fillValue)
		var gradient *element
		for _, attribute := range path.children(aaptNamespace, "attr") {
			attributeName, _ := attribute.attribute("", "name")
			if attributeName == "android:fillColor" {
				if candidate := attribute.children("", "gradient"); len(candidate) != 0 {
					gradient = candidate[0]
				}
			}
		}
		if gradient != nil {
			brush, err := gradientXAML(gradient, 10)
			if err != nil {
				return "", err
			}
			body = append(body,
				fmt.Sprintf(`      <Path Data="%s">`, escapeAttribute(pathData)),
				"        <Path.Fill>", brush, "        </Path.Fill>", "      </Path>",
			)
			continue
		}
		fillAttribute := ""
		if fill != "" {
			fillAttribute = fmt.Sprintf(` Fill="%s"`, escapeAttribute(fill))
		}
		body = append(body, fmt.Sprintf(`      <Path%s Data="%s" />`, fillAttribute, escapeAttribute(pathData)))
	}

	key := ""
	if name != "" {
		key = fmt.Sprintf(` x:Key="%s"`, escapeAttribute(name))
	}
	inner := strings.Join(body, "\n")
	if size != nil {
		return fmt.Sprintf("<Viewbox%s Width=\"%s\" Height=\"%s\">\n  <Canvas Width=\"%s\" Height=\"%s\">\n%s\n  </Canvas>\n</Viewbox>",
			key, floatString(*size), floatString(*size), floatString(width), floatString(height), inner), nil
	}
	return fmt.Sprintf("<Canvas%s Width=\"%s\" Height=\"%s\">\n%s\n</Canvas>",
		key, floatString(width), floatString(height), inner), nil
}

func parseArguments(arguments []string) (options, error) {
	var result options
	for index := 0; index < len(arguments); index++ {
		argument := arguments[index]
		nextValue := func(name string) (string, error) {
			index++
			if index >= len(arguments) {
				return "", fmt.Errorf("%s requires a value", name)
			}
			return arguments[index], nil
		}
		switch {
		case argument == "--name":
			value, err := nextValue(argument)
			if err != nil {
				return result, err
			}
			result.name = value
		case strings.HasPrefix(argument, "--name="):
			result.name = strings.TrimPrefix(argument, "--name=")
		case argument == "--size":
			value, err := nextValue(argument)
			if err != nil {
				return result, err
			}
			parsed, err := strconv.ParseFloat(value, 64)
			if err != nil {
				return result, fmt.Errorf("invalid --size %q: %w", value, err)
			}
			result.size = &parsed
		case strings.HasPrefix(argument, "--size="):
			value := strings.TrimPrefix(argument, "--size=")
			parsed, err := strconv.ParseFloat(value, 64)
			if err != nil {
				return result, fmt.Errorf("invalid --size %q: %w", value, err)
			}
			result.size = &parsed
		case strings.HasPrefix(argument, "-"):
			return result, fmt.Errorf("unknown option: %s", argument)
		case result.input == "":
			result.input = argument
		default:
			return result, fmt.Errorf("unexpected argument: %s", argument)
		}
	}
	if result.input == "" {
		return result, errors.New("usage: vd2xaml <input.xml> [--name Foo] [--size 18]")
	}
	return result, nil
}

func run(arguments []string) error {
	options, err := parseArguments(arguments)
	if err != nil {
		return err
	}
	output, err := convert(options.input, options.name, options.size)
	if err != nil {
		return err
	}
	fmt.Println(output)
	return nil
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
}
