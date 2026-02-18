# GDScript to Godot XML Documentation Generator

Automatically parse GDScript comment documentation (`##`) and generate standard Godot XML documentation files.

## Features

- Extracts class-level documentation
- Parses signals with parameters and descriptions
- Extracts methods with parameters, return types, and descriptions
- Handles member variables (exported and regular)
- Parses constants
- Generates properly formatted Godot XML documentation
- Preserves BBCode formatting (`[br]`, `[method]`, etc.)

## Usage

### Command Line

```bash
python gdscript_to_xml.py <input.gd>
```

The XML file will automatically be saved to `doc_classes/<input-file-name>.xml`. The `doc_classes/` directory will be created if it doesn't exist, and any existing file will be overwritten.

### Examples

```bash
# Generate doc_classes/aidedecam.xml from aidedecam.gd
python gdscript_to_xml.py aidedecam.gd

# Generate doc_classes/my_script.xml from path/to/my_script.gd
python gdscript_to_xml.py path/to/my_script.gd
```

### As a Module

```python
from gdscript_to_xml import convert_gdscript_to_xml
from pathlib import Path

# Convert and save to custom location
xml_content = convert_gdscript_to_xml('aidedecam.gd', 'custom/path/output.xml')

# Or let it auto-save to doc_classes/ directory
from gdscript_to_xml import GDScriptParser, GodotXMLGenerator

parser = GDScriptParser('aidedecam.gd')
parsed_data = parser.parse()
generator = GodotXMLGenerator(parsed_data)
xml_content = generator.generate()

# Save manually
Path('doc_classes').mkdir(exist_ok=True)
Path('doc_classes/aidedecam.xml').write_text(xml_content)
```

## Documentation Format

The parser recognizes GDScript documentation comments using the `##` syntax:

### Class Documentation

```gdscript
extends Node

## Brief description on first line.
##
## Detailed description can span multiple lines.
## Add as many lines as needed.
```

### Signals

```gdscript
## Signal description here.
signal signal_name

## Signal with parameters.
signal signal_with_params(param1: String, param2: int)
```

### Methods

```gdscript
## Method description here.
## Can span multiple lines.
func method_name(param1: String, param2: int = 0) -> bool:
    return true
```

### Member Variables

```gdscript
## Variable description.
var public_var: String

## Exported variable with description.
@export var exported_var: int = 10
```

### Constants

```gdscript
## Constant description.
const MAX_VALUE: int = 100
```

## Output Format

The script generates XML files compatible with Godot's documentation system:

```xml
<?xml version="1.0" ?>
<class name="ClassName" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="../class.xsd">
  <brief_description>
    Brief description here.
  </brief_description>
  <description>
    Detailed description here.
  </description>
  <tutorials>
  </tutorials>
  <methods>
    <method name="method_name">
      <return type="void"/>
      <param index="0" name="param1" type="String"/>
      <description>
        Method description.
      </description>
    </method>
  </methods>
  <signals>
    <signal name="signal_name">
      <description>
        Signal description.
      </description>
    </signal>
  </signals>
</class>
```

## Notes

- Documentation must use `##` (double hash) comments
- Single `#` comments are ignored
- Blank lines separate class documentation from subsequent element documentation
- The parser attempts to infer types from GDScript type hints
- BBCode formatting in comments is preserved in the XML output
- Private methods (starting with `_`) are excluded except for special methods like `_ready()`

## Requirements

- Python 3.6 or higher
- No external dependencies (uses only standard library)

## Integration with Godot

Place the generated XML files in your Godot project's documentation directory. Godot will automatically use them to populate the built-in help system.

Typical location: `res://doc_classes/` or alongside your scripts.
