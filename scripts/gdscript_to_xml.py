#!/usr/bin/env python3
"""
GDScript to Godot XML Documentation Generator

Parses GDScript files with doc comments (## style) and generates
XML documentation compatible with Godot's documentation system.
"""

import re
import xml.etree.ElementTree as ET
from xml.dom import minidom
from pathlib import Path
from typing import List, Dict, Optional, Tuple


class GDScriptParser:
    """Parses GDScript files to extract documentation information."""
    
    def __init__(self, filepath: str):
        self.filepath = Path(filepath)
        self.content = self.filepath.read_text(encoding='utf-8')
        self.lines = self.content.split('\n')
        
    def parse(self) -> Dict:
        """Parse the GDScript file and extract documentation."""
        doc = {
            'class_name': self._get_class_name(),
            'brief_description': '',
            'description': '',
            'methods': [],
            'signals': [],
            'members': [],
            'constants': []
        }
        
        # Extract class-level documentation
        class_docs = self._extract_class_docs()
        if class_docs:
            lines = class_docs.strip().split('\n')
            if lines:
                doc['brief_description'] = lines[0]
                if len(lines) > 1:
                    doc['description'] = '\n'.join(lines[1:]).strip()
        
        # Extract signals
        doc['signals'] = self._extract_signals()
        
        # Extract methods
        doc['methods'] = self._extract_methods()
        
        # Extract member variables
        doc['members'] = self._extract_members()
        
        # Extract constants
        doc['constants'] = self._extract_constants()
        
        return doc
    
    def _get_class_name(self) -> str:
        """Extract class name from the file."""
        # Try to find class_name declaration
        for line in self.lines:
            match = re.match(r'class_name\s+(\w+)', line.strip())
            if match:
                return match.group(1)
        
        # Fall back to filename without extension
        return self.filepath.stem
    
    def _extract_class_docs(self) -> str:
        """Extract class-level documentation comments."""
        docs = []
        in_class_doc = False
        found_extends = False
        
        for idx, line in enumerate(self.lines):
            stripped = line.strip()
            
            # Track when we've seen extends
            if stripped.startswith('extends') or stripped.startswith('class_name'):
                found_extends = True
                continue
            
            # Start collecting docs after extends
            if found_extends and stripped.startswith('##'):
                in_class_doc = True
                # Remove ## and leading/trailing whitespace
                doc_text = stripped[2:].strip()
                docs.append(doc_text)
            elif in_class_doc and stripped.startswith('##'):
                # Continue collecting consecutive ## comments
                doc_text = stripped[2:].strip()
                docs.append(doc_text)
            elif in_class_doc and stripped == '':
                # Empty line ends class documentation
                break
            elif in_class_doc and stripped and not stripped.startswith('#'):
                # Stop when we hit actual code (signal, var, func, etc)
                break
        
        return '\n'.join(docs)
    
    def _extract_doc_comment(self, line_idx: int) -> str:
        """Extract documentation comment immediately above a given line index."""
        docs = []
        idx = line_idx - 1
        blank_line_seen = False
        
        # Look backwards for doc comments (only consecutive ones)
        while idx >= 0:
            line = self.lines[idx].strip()
            if line.startswith('##'):
                # If we saw a blank line, stop here (docs aren't consecutive)
                if blank_line_seen:
                    break
                doc_text = line[2:].strip()
                docs.insert(0, doc_text)
                idx -= 1
            elif line == '':
                # Mark that we've seen a blank line, but continue for one more check
                blank_line_seen = True
                idx -= 1
            else:
                # Non-doc, non-empty line
                break
        
        return '\n'.join(docs)
    
    def _extract_signals(self) -> List[Dict]:
        """Extract signal definitions with their documentation."""
        signals = []
        
        for idx, line in enumerate(self.lines):
            match = re.match(r'signal\s+(\w+)(\(.*?\))?', line.strip())
            if match:
                signal_name = match.group(1)
                params_str = match.group(2) or '()'
                
                # Parse parameters
                params = self._parse_parameters(params_str)
                
                # Get documentation
                doc = self._extract_doc_comment(idx)
                
                signals.append({
                    'name': signal_name,
                    'params': params,
                    'description': doc
                })
        
        return signals
    
    def _extract_methods(self) -> List[Dict]:
        """Extract method definitions with their documentation."""
        methods = []
        
        for idx, line in enumerate(self.lines):
            # Match function declarations
            match = re.match(r'func\s+(\w+)\s*\((.*?)\)\s*(?:->\s*(\w+))?', line.strip())
            if match:
                method_name = match.group(1)
                params_str = match.group(2) or ''
                return_type = match.group(3) or 'void'
                
                # Skip private methods (starting with _) unless they're special like _ready
                if method_name.startswith('_') and method_name not in ['_ready', '_process', '_physics_process', '_input', '_unhandled_input']:
                    continue
                
                # Parse parameters
                params = self._parse_parameters(f'({params_str})')
                
                # Get documentation
                doc = self._extract_doc_comment(idx)
                
                methods.append({
                    'name': method_name,
                    'params': params,
                    'return_type': return_type,
                    'description': doc
                })
        
        return methods
    
    def _extract_members(self) -> List[Dict]:
        """Extract member variable definitions."""
        members = []
        
        for idx, line in enumerate(self.lines):
            # Match var declarations
            match = re.match(r'(?:@export\s+)?var\s+(\w+)(?:\s*:\s*(\w+))?(?:\s*=\s*(.+?))?(?:\s*#.*)?$', line.strip())
            if match:
                var_name = match.group(1)
                var_type = match.group(2) or 'Variant'
                default_value = match.group(3)
                
                # Skip private variables
                if var_name.startswith('_'):
                    continue
                
                # Get documentation
                doc = self._extract_doc_comment(idx)
                
                # Check for @export
                is_exported = '@export' in self.lines[max(0, idx-1)] or '@export' in line
                
                members.append({
                    'name': var_name,
                    'type': var_type,
                    'default': default_value,
                    'description': doc,
                    'exported': is_exported
                })
        
        return members
    
    def _extract_constants(self) -> List[Dict]:
        """Extract constant definitions."""
        constants = []
        
        for idx, line in enumerate(self.lines):
            # Match const declarations
            match = re.match(r'const\s+(\w+)(?:\s*:\s*(\w+))?\s*=\s*(.+?)(?:\s*#.*)?$', line.strip())
            if match:
                const_name = match.group(1)
                const_type = match.group(2) or 'Variant'
                const_value = match.group(3)
                
                # Get documentation
                doc = self._extract_doc_comment(idx)
                
                constants.append({
                    'name': const_name,
                    'type': const_type,
                    'value': const_value,
                    'description': doc
                })
        
        return constants
    
    def _parse_parameters(self, params_str: str) -> List[Dict]:
        """Parse parameter string into list of parameter dictionaries."""
        params = []
        
        # Remove parentheses
        params_str = params_str.strip('()')
        if not params_str:
            return params
        
        # Split by comma (simple split, doesn't handle complex nested types)
        param_parts = params_str.split(',')
        
        for part in param_parts:
            part = part.strip()
            if not part:
                continue
            
            # Parse parameter: name: type = default
            match = re.match(r'(\w+)(?:\s*:\s*(\w+))?(?:\s*=\s*(.+))?', part)
            if match:
                param_name = match.group(1)
                param_type = match.group(2) or 'Variant'
                param_default = match.group(3)
                
                params.append({
                    'name': param_name,
                    'type': param_type,
                    'default': param_default
                })
        
        return params


class GodotXMLGenerator:
    """Generates Godot XML documentation from parsed GDScript data."""
    
    def __init__(self, parsed_data: Dict):
        self.data = parsed_data
        
    def generate(self) -> str:
        """Generate XML documentation string."""
        root = ET.Element('class')
        root.set('name', self.data['class_name'])
        root.set('xmlns:xsi', 'http://www.w3.org/2001/XMLSchema-instance')
        root.set('xsi:noNamespaceSchemaLocation', '../class.xsd')
        
        # Brief description
        brief = ET.SubElement(root, 'brief_description')
        brief.text = self.data['brief_description']
        
        # Description
        description = ET.SubElement(root, 'description')
        description.text = self.data['description']
        
        # Tutorials (empty for now)
        ET.SubElement(root, 'tutorials')
        
        # Methods
        if self.data['methods']:
            methods = ET.SubElement(root, 'methods')
            for method in self.data['methods']:
                self._add_method(methods, method)
        
        # Signals
        if self.data['signals']:
            signals = ET.SubElement(root, 'signals')
            for signal in self.data['signals']:
                self._add_signal(signals, signal)
        
        # Members
        if self.data['members']:
            members = ET.SubElement(root, 'members')
            for member in self.data['members']:
                self._add_member(members, member)
        
        # Constants
        if self.data['constants']:
            constants = ET.SubElement(root, 'constants')
            for constant in self.data['constants']:
                self._add_constant(constants, constant)
        
        # Convert to pretty XML string
        return self._prettify(root)
    
    def _add_method(self, parent: ET.Element, method: Dict) -> None:
        """Add a method element."""
        method_elem = ET.SubElement(parent, 'method')
        method_elem.set('name', method['name'])
        
        # Return type
        return_elem = ET.SubElement(method_elem, 'return')
        return_elem.set('type', method['return_type'])
        
        # Parameters
        for param in method['params']:
            param_elem = ET.SubElement(method_elem, 'param')
            param_elem.set('index', str(method['params'].index(param)))
            param_elem.set('name', param['name'])
            param_elem.set('type', param['type'])
            if param['default']:
                param_elem.set('default', param['default'])
        
        # Description
        desc_elem = ET.SubElement(method_elem, 'description')
        desc_elem.text = method['description']
    
    def _add_signal(self, parent: ET.Element, signal: Dict) -> None:
        """Add a signal element."""
        signal_elem = ET.SubElement(parent, 'signal')
        signal_elem.set('name', signal['name'])
        
        # Parameters
        for param in signal['params']:
            param_elem = ET.SubElement(signal_elem, 'param')
            param_elem.set('index', str(signal['params'].index(param)))
            param_elem.set('name', param['name'])
            param_elem.set('type', param['type'])
        
        # Description
        desc_elem = ET.SubElement(signal_elem, 'description')
        desc_elem.text = signal['description']
    
    def _add_member(self, parent: ET.Element, member: Dict) -> None:
        """Add a member element."""
        member_elem = ET.SubElement(parent, 'member')
        member_elem.set('name', member['name'])
        member_elem.set('type', member['type'])
        if member['default']:
            member_elem.set('default', member['default'])
        
        # Description
        member_elem.text = member['description']
    
    def _add_constant(self, parent: ET.Element, constant: Dict) -> None:
        """Add a constant element."""
        const_elem = ET.SubElement(parent, 'constant')
        const_elem.set('name', constant['name'])
        const_elem.set('value', constant['value'])
        
        # Description
        const_elem.text = constant['description']
    
    def _prettify(self, elem: ET.Element) -> str:
        """Return a pretty-printed XML string."""
        rough_string = ET.tostring(elem, encoding='unicode')
        reparsed = minidom.parseString(rough_string)
        return reparsed.toprettyxml(indent='  ')


def convert_gdscript_to_xml(gdscript_path: str, output_path: Optional[str] = None) -> str:
    """
    Convert a GDScript file to Godot XML documentation.
    
    Args:
        gdscript_path: Path to the .gd file
        output_path: Optional path to save the XML (defaults to same name with .xml)
    
    Returns:
        The generated XML string
    """
    # Parse the GDScript file
    parser = GDScriptParser(gdscript_path)
    parsed_data = parser.parse()
    
    # Generate XML
    generator = GodotXMLGenerator(parsed_data)
    xml_content = generator.generate()
    
    # Save if output path specified
    if output_path:
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(xml_content)
    
    return xml_content


if __name__ == '__main__':
    import sys
    
    if len(sys.argv) < 2:
        print("Usage: python gdscript_to_xml.py <gdscript_file>")
        sys.exit(1)
    
    input_file = sys.argv[1]
    
    # Create docs directory if it doesn't exist
    docs_dir = Path(input_file).parent / 'doc_classes'
    docs_dir.mkdir(exist_ok=True)
    
    # Always save to docs/<input-file-name>.xml
    input_filename = Path(input_file).stem
    output_file = docs_dir / f"{input_filename}.xml"
    
    xml = convert_gdscript_to_xml(input_file, output_file)
    print(f"Generated XML documentation: {output_file}")
