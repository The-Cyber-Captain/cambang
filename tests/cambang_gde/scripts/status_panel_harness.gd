extends SceneTree

const DEFAULT_VIEWPORT_SIZE := Vector2i(1280, 720)
const DEFAULT_SCREENSHOT_VIEWPORT_SIZE := Vector2i(1400, 1600)
const SnapshotValidator = preload("res://support/snapshot_schema_validator.gd")

func _initialize() -> void:
	var parse_result := _parse_cli_args(OS.get_cmdline_user_args())
	if not bool(parse_result.get("ok", false)):
		_printerr(str(parse_result.get("error", "usage: -- <fixture.json> [screenshot.png] [--window-width <px>] [--window-height <px>]")))
		_printerr("usage: -- <fixture.json> [screenshot.png] [--window-width <px>] [--window-height <px>]")
		_printerr("note: screenshot capture requires a headed/windowed run; semantic-only runs remain headless-compatible")
		quit(2)
		return

	var fixture_path := str(parse_result.get("fixture_path", ""))
	var screenshot_path := str(parse_result.get("screenshot_path", ""))
	var window_size := _resolve_window_size(parse_result, screenshot_path != "")

	if screenshot_path != "":
		var screenshot_preflight_error := _validate_screenshot_mode()
		if screenshot_preflight_error != "":
			_printerr(screenshot_preflight_error)
			quit(2)
			return

	var fixture := _load_fixture(fixture_path)
	if fixture.is_empty():
		quit(2)
		return

	var payload: Variant = fixture.get("payload", null)
	var schema_errors: Array[String] = []
	var fixture_kind := str(fixture.get("fixture_kind", "authoritative_snapshot"))

	if fixture_kind == "authoritative_snapshot":
		if typeof(payload) == TYPE_DICTIONARY:
			schema_errors = SnapshotValidator.validate_snapshot(payload)
		else:
			schema_errors.append("payload must be Dictionary")

	print("HARNESS schema_errors: %s" % [schema_errors])

	var expected_validity := str(fixture.get("expected_validity", "valid"))
	var should_run_projector := bool(fixture.get("should_run_projector", true))
	var expected_panel_outcome: Dictionary = fixture.get("expected_panel_outcome", {})
	var adversarial_projection: Dictionary = fixture.get("adversarial_projection", {})
	var provider_mode := str(fixture.get("provider_mode", "fixture"))
	var initial_expanded_row_ids: Array = fixture.get("initial_expanded_row_ids", [])

	var window := Window.new()
	window.title = "status_panel_harness"
	window.size = window_size
	window.mode = Window.MODE_WINDOWED
	window.visible = true
	get_root().add_child(window)

	var panel_script: Variant = load("res://addons/cambang/cambang_status_panel.gd")
	if panel_script == null:
		_printerr("failed to load status panel script")
		quit(1)
		return

	if not panel_script is GDScript:
		_printerr("loaded panel script is not a GDScript resource")
		quit(1)
		return

	var panel: Variant = panel_script.new()
	if panel == null:
		_printerr("failed to instantiate status panel script")
		quit(1)
		return

	panel.name = "CamBANGStatusPanel"
	panel.set_anchors_preset(Control.PRESET_FULL_RECT)
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	window.add_child(panel)

	await process_frame
	panel.call("_disconnect_server")

	var contract_gaps: Array = []
	var projection_gaps: Array = []
	var is_schema_valid := schema_errors.is_empty()
	var is_projection_compatible := true
	if fixture_kind == "authoritative_snapshot":
		var compat: Dictionary = _compute_runtime_compat(panel, payload)
		contract_gaps = compat.get("contract_gaps", [])
		projection_gaps = compat.get("projection_gaps", [])
		is_projection_compatible = projection_gaps.is_empty()

	print("HARNESS contract_gaps: %s" % [contract_gaps])
	print("HARNESS projection_gaps: %s" % [projection_gaps])
	print("HARNESS schema_valid=%s projection_compatible=%s" % [is_schema_valid, is_projection_compatible])

	if expected_validity == "valid" and not is_schema_valid:
		_printerr("valid fixture failed schema validation: %s" % [schema_errors])
		quit(1)
		return

	if expected_validity == "invalid" and schema_errors.is_empty():
		_printerr("expected invalid fixture but schema passed")
		quit(1)
		return

	var active_panel = null
	var rendered_model = null
	var snapshot_reading: Dictionary = {}

	if fixture_kind == "continuity_no_snapshot":
		var continuity_prelude_payload: Variant = fixture.get("continuity_prelude_payload", null)
		var continuity_result := _build_continuity_no_snapshot_model(panel, continuity_prelude_payload, provider_mode)
		if not bool(continuity_result.get("ok", false)):
			_printerr(str(continuity_result.get("error", "continuity_no_snapshot setup failed")))
			quit(1)
			return
		rendered_model = continuity_result.get("rendered_model", null)
		snapshot_reading = continuity_result.get("snapshot_reading", {})
	else:
		snapshot_reading = panel.call("_read_snapshot", payload)
		if not should_run_projector:
			active_panel = panel.call("_build_runtime_compat_fallback_panel", contract_gaps, projection_gaps)
			rendered_model = panel.call("_compose_presented_panel_model", active_panel, false, {})
		elif not is_schema_valid or not is_projection_compatible:
			active_panel = panel.call("_build_runtime_compat_fallback_panel", contract_gaps, projection_gaps)
			rendered_model = panel.call("_compose_presented_panel_model", active_panel, false, {})
		else:
			active_panel = panel.call("_project_snapshot_to_panel_model", payload, provider_mode)
			var snapshot_meta: Dictionary = panel.call("_extract_authoritative_snapshot_meta", payload)
			rendered_model = panel.call("_compose_presented_panel_model", active_panel, true, snapshot_meta)
			rendered_model = _apply_adversarial_projection(rendered_model, adversarial_projection, panel)

	panel.call("_apply_snapshot_read", snapshot_reading)
	if not initial_expanded_row_ids.is_empty():
		var expanded_with_ancestors := _expand_rows_with_ancestors(rendered_model, initial_expanded_row_ids)
		panel.call("apply_fixture_expanded_rows", expanded_with_ancestors)
	panel.call("_render_panel_and_maybe_dump", rendered_model, _resolve_render_snapshot(payload))

	await process_frame
	await process_frame

	if screenshot_path != "":
		var capture_error := _capture_window_png(window, screenshot_path)
		if capture_error != "":
			_printerr(capture_error)
			quit(2)
			return

	var row_ids := _collect_entry_ids(rendered_model)
	print("HARNESS row_ids: %s" % [row_ids])
	var visible_row_ids := _collect_visible_row_ids(panel)
	print("HARNESS visible_row_ids: %s" % [visible_row_ids])

	var rendered_contract_gap_rows := 0
	for id in row_ids:
		if typeof(id) == TYPE_STRING and id.ends_with("/contract_gaps"):
			rendered_contract_gap_rows += 1

	print("HARNESS rendered_contract_gap_rows: %d" % rendered_contract_gap_rows)

	_dump_rendered_entries(rendered_model)

	var failures := _evaluate_expectations(
		expected_panel_outcome,
		snapshot_reading,
		contract_gaps,
		projection_gaps,
		row_ids,
		visible_row_ids,
		rendered_model
	)
	if not failures.is_empty():
		for failure in failures:
			_printerr(str(failure))
		quit(1)
		return

	var observed_class := _classify_observed_outcome(contract_gaps, projection_gaps, row_ids)

	if expected_validity == "valid":
		print("OK: valid fixture behaved as expected (%s): %s" % [observed_class, fixture_path])
	else:
		print("OK: expected-invalid fixture was rejected/handled as expected (%s): %s" % [observed_class, fixture_path])

	quit(0)


func _parse_cli_args(args: PackedStringArray) -> Dictionary:
	var fixture_path := ""
	var screenshot_path := ""
	var window_width: Variant = null
	var window_height: Variant = null
	var index := 0

	while index < args.size():
		var arg := args[index]
		match arg:
			"--window-width":
				index += 1
				if index >= args.size():
					return {
						"ok": false,
						"error": "missing value for --window-width",
					}
				var parsed_width := _parse_dimension_arg(args[index], "--window-width")
				if not bool(parsed_width.get("ok", false)):
					return parsed_width
				window_width = parsed_width.get("value")
			"--window-height":
				index += 1
				if index >= args.size():
					return {
						"ok": false,
						"error": "missing value for --window-height",
					}
				var parsed_height := _parse_dimension_arg(args[index], "--window-height")
				if not bool(parsed_height.get("ok", false)):
					return parsed_height
				window_height = parsed_height.get("value")
			_:
				if arg.begins_with("--"):
					return {
						"ok": false,
						"error": "unknown option: %s" % arg,
					}
				if fixture_path == "":
					fixture_path = arg
				elif screenshot_path == "":
					screenshot_path = arg
				else:
					return {
						"ok": false,
						"error": "unexpected extra positional argument: %s" % arg,
					}
		index += 1

	if fixture_path == "":
		return {
			"ok": false,
			"error": "missing fixture path",
		}

	return {
		"ok": true,
		"fixture_path": fixture_path,
		"screenshot_path": screenshot_path,
		"window_width": window_width,
		"window_height": window_height,
	}


func _parse_dimension_arg(raw_value: String, option_name: String) -> Dictionary:
	if raw_value == "":
		return {
			"ok": false,
			"error": "%s requires a positive integer" % option_name,
		}

	if not raw_value.is_valid_int():
		return {
			"ok": false,
			"error": "%s requires a positive integer; got %s" % [option_name, raw_value],
		}

	var value := int(raw_value)
	if value <= 0:
		return {
			"ok": false,
			"error": "%s requires a positive integer; got %d" % [option_name, value],
		}

	return {
		"ok": true,
		"value": value,
	}


func _resolve_window_size(parse_result: Dictionary, is_screenshot_run: bool) -> Vector2i:
	var has_explicit_width := parse_result.get("window_width", null) != null
	var has_explicit_height := parse_result.get("window_height", null) != null

	var window_size := DEFAULT_VIEWPORT_SIZE
	if is_screenshot_run and not has_explicit_width and not has_explicit_height:
		window_size = DEFAULT_SCREENSHOT_VIEWPORT_SIZE

	if has_explicit_width:
		window_size.x = int(parse_result.get("window_width"))
	if has_explicit_height:
		window_size.y = int(parse_result.get("window_height"))

	return window_size


func _load_fixture(path: String) -> Dictionary:
	var text := FileAccess.get_file_as_string(path)
	if text == "":
		_printerr("failed to read fixture: %s" % path)
		return {}

	var json := JSON.new()
	var err := json.parse(text)
	if err != OK:
		_printerr("fixture parse error at line %d: %s" % [json.get_error_line(), json.get_error_message()])
		return {}

	if typeof(json.data) != TYPE_DICTIONARY:
		_printerr("fixture root must be a Dictionary")
		return {}
	return json.data


func _compute_runtime_compat(panel: Object, payload: Variant) -> Dictionary:
	if payload == null:
		return {
			"ok": false,
			"contract_gaps": ["Contract gap: payload is null."],
			"projection_gaps": [],
		}
	if typeof(payload) != TYPE_DICTIONARY:
		return {
			"ok": false,
			"contract_gaps": ["Contract gap: payload must be Dictionary; got type=%d." % typeof(payload)],
			"projection_gaps": [],
		}
	return panel.call("_check_snapshot_runtime_compat", payload)


func _resolve_render_snapshot(payload: Variant) -> Variant:
	if payload == null:
		return null
	if typeof(payload) != TYPE_DICTIONARY:
		return null
	return payload


func _build_continuity_no_snapshot_model(panel: Object, continuity_prelude_payload: Variant, provider_mode: String) -> Dictionary:
	if continuity_prelude_payload != null and typeof(continuity_prelude_payload) != TYPE_DICTIONARY:
		return {
			"ok": false,
			"error": "continuity_prelude_payload must be Dictionary or null",
		}

	if typeof(continuity_prelude_payload) == TYPE_DICTIONARY:
		var prelude_payload: Dictionary = continuity_prelude_payload
		var prelude_schema_errors: Array[String] = SnapshotValidator.validate_snapshot(prelude_payload)
		if not prelude_schema_errors.is_empty():
			return {
				"ok": false,
				"error": "continuity_prelude_payload failed schema validation: %s" % [prelude_schema_errors],
			}

		var prelude_compat: Dictionary = panel.call("_check_snapshot_runtime_compat", prelude_payload)
		if not bool(prelude_compat.get("ok", false)):
			return {
				"ok": false,
				"error": "continuity_prelude_payload failed runtime compatibility: %s" % [prelude_compat],
			}

		var prelude_active_panel = panel.call("_project_snapshot_to_panel_model", prelude_payload, provider_mode)
		var prelude_snapshot_meta: Dictionary = panel.call("_extract_authoritative_snapshot_meta", prelude_payload)
		panel.call("_set_last_active_panel_state", prelude_active_panel, true, prelude_snapshot_meta)
		panel.call("_compose_presented_panel_model", prelude_active_panel, true, prelude_snapshot_meta)

	var nil_panel = panel.call("_build_nil_panel_model", "No published snapshot yet.")
	panel.call("_set_last_active_panel_state", nil_panel, false, {})
	var rendered_model = panel.call("_compose_presented_panel_model", nil_panel, false, {})
	var snapshot_reading: Dictionary = panel.call("_read_snapshot", null)

	return {
		"ok": true,
		"rendered_model": rendered_model,
		"snapshot_reading": snapshot_reading,
	}


func _apply_adversarial_projection(rendered_model: Variant, config: Dictionary, panel: Object) -> Variant:
	if rendered_model == null:
		return rendered_model
	if config.is_empty():
		return rendered_model

	if not rendered_model is Object:
		return rendered_model

	var strip_ids_variant: Variant = config.get("strip_materialized_native_ids", [])
	if typeof(strip_ids_variant) != TYPE_ARRAY:
		return rendered_model
	var strip_ids_raw: Array = strip_ids_variant
	if strip_ids_raw.is_empty():
		return rendered_model

	var strip_native_ids := {}
	for raw_id in strip_ids_raw:
		var native_id := int(raw_id)
		if native_id > 0:
			strip_native_ids[native_id] = true
	if strip_native_ids.is_empty():
		return rendered_model

	var entries_variant: Variant = rendered_model.get("entries")
	if typeof(entries_variant) != TYPE_ARRAY:
		return rendered_model
	var source_entries: Array = entries_variant
	if source_entries.is_empty():
		return rendered_model

	var stripped_row_ids := {}
	for raw_entry in source_entries:
		if raw_entry == null:
			continue
		var materialized_native_id := int(raw_entry.materialized_native_id)
		if strip_native_ids.has(materialized_native_id):
			var entry_id := str(raw_entry.id)
			if entry_id != "":
				stripped_row_ids[entry_id] = true

	if stripped_row_ids.is_empty():
		return rendered_model

	var changed := true
	while changed:
		changed = false
		for raw_entry in source_entries:
			if raw_entry == null:
				continue
			var entry_id := str(raw_entry.id)
			if entry_id == "" or stripped_row_ids.has(entry_id):
				continue
			var parent_id := str(raw_entry.parent_id)
			if parent_id != "" and stripped_row_ids.has(parent_id):
				stripped_row_ids[entry_id] = true
				changed = true

	var filtered_entries: Array = []
	for raw_entry in source_entries:
		if raw_entry == null:
			continue
		var entry_id := str(raw_entry.id)
		if entry_id != "" and stripped_row_ids.has(entry_id):
			continue
		filtered_entries.append(raw_entry)

	source_entries.clear()
	for kept_entry in filtered_entries:
		source_entries.append(kept_entry)
	panel.call("_ensure_expandability", rendered_model)
	return rendered_model


func _validate_screenshot_mode() -> String:
	var display_name := DisplayServer.get_name()
	if display_name == "headless":
		return "screenshot capture requires a headed/windowed run; current display server is headless, so window rendering/capture is unavailable. Rerun without --headless."
	return ""


func _capture_window_png(window: Window, screenshot_path: String) -> String:
	var texture := window.get_texture()
	if texture == null:
		return "screenshot capture requires a headed/windowed run; window texture capture is unavailable in the current renderer/display server. Rerun without --headless using a real window/render path."

	var image: Image = texture.get_image()
	if image == null:
		return "screenshot capture requires a headed/windowed run; failed to read pixels from the real window texture in the current renderer/display server. Rerun without --headless using a real window/render path."

	var err := image.save_png(screenshot_path)
	if err != OK:
		return "failed to save screenshot: %s (err=%d)" % [screenshot_path, err]
	return ""


func _collect_entry_ids(model: Variant) -> Array[String]:
	var ids: Array[String] = []

	if model == null:
		return ids

	var entries: Array = _extract_entries(model)
	for entry in entries:
		var entry_id := _extract_entry_id(entry)
		if entry_id != "":
			ids.append(entry_id)

	return ids


func _expand_rows_with_ancestors(model: Variant, requested_row_ids: Array) -> Array[String]:
	var expanded_set := {}
	var parent_by_id := {}
	if model != null:
		for raw_entry in model.entries:
			if raw_entry == null:
				continue
			var entry_id := str(raw_entry.id)
			if entry_id.is_empty():
				continue
			parent_by_id[entry_id] = str(raw_entry.parent_id)

	for raw_row_id in requested_row_ids:
		var row_id := str(raw_row_id)
		if row_id.is_empty():
			continue
		var current_id := row_id
		while current_id != "":
			if expanded_set.has(current_id):
				break
			expanded_set[current_id] = true
			current_id = str(parent_by_id.get(current_id, ""))

	var expanded_rows: Array[String] = []
	for row_id in expanded_set.keys():
		expanded_rows.append(str(row_id))
	return expanded_rows


func _extract_entries(model: Variant) -> Array:
	if model == null:
		return []

	if typeof(model) == TYPE_DICTIONARY:
		return model.get("entries", [])

	if model is Object:
		var property_list: Array = model.get_property_list()
		for prop in property_list:
			if str(prop.get("name", "")) == "entries":
				return model.get("entries")

	return []


func _extract_entry_id(entry: Variant) -> String:
	if entry == null:
		return ""

	if typeof(entry) == TYPE_DICTIONARY:
		return str(entry.get("id", ""))

	if entry is Object:
		var property_list: Array = entry.get_property_list()
		for prop in property_list:
			if str(prop.get("name", "")) == "id":
				return str(entry.get("id"))

	return ""


func _dump_rendered_entries(model: Variant) -> void:
	var entries: Array = _extract_entries(model)
	print("HARNESS rendered entries:")
	for entry in entries:
		var entry_id := _extract_entry_id(entry)
		var label := _extract_entry_field(entry, "label")
		var parent_id := _extract_entry_field(entry, "parent_id")
		var counters := _extract_entry_counters(entry)
		var badges := _extract_entry_badges(entry)
		var info_lines := _extract_entry_info_lines(entry)
		print("  id=%s parent=%s label=%s counters=%s badges=%s info=%s" % [
			entry_id,
			parent_id,
			label,
			counters,
			badges,
			info_lines,
		])


func _extract_entry_field(entry: Variant, field_name: String) -> String:
	if entry == null:
		return ""

	if typeof(entry) == TYPE_DICTIONARY:
		return str(entry.get(field_name, ""))

	if entry is Object:
		var property_list: Array = entry.get_property_list()
		for prop in property_list:
			if str(prop.get("name", "")) == field_name:
				return str(entry.get(field_name))

	return ""


func _extract_entry_counters(entry: Variant) -> Dictionary:
	var counters: Dictionary = {}
	var raw_counters: Array = _extract_array_field(entry, "counters")
	for raw_counter in raw_counters:
		var name: Variant = _extract_variant_field(raw_counter, "name")
		if name == null:
			continue
		counters[str(name)] = int(_extract_variant_field(raw_counter, "value", 0))
	return counters


func _extract_entry_badges(entry: Variant) -> Array[String]:
	var badges: Array[String] = []
	var raw_badges: Array = _extract_array_field(entry, "badges")
	for raw_badge in raw_badges:
		var label: Variant = _extract_variant_field(raw_badge, "label")
		if label == null:
			continue
		badges.append(str(label))
	return badges


func _extract_entry_info_lines(entry: Variant) -> Array[String]:
	var lines: Array[String] = []
	var raw_lines: Array = _extract_array_field(entry, "info_lines")
	for raw_line in raw_lines:
		lines.append(str(raw_line))
	return lines


func _extract_array_field(entry: Variant, field_name: String) -> Array:
	var value: Variant = _extract_variant_field(entry, field_name, [])
	return value if typeof(value) == TYPE_ARRAY else []


func _extract_variant_field(entry: Variant, field_name: String, fallback: Variant = null) -> Variant:
	if entry == null:
		return fallback
	if typeof(entry) == TYPE_DICTIONARY:
		return entry.get(field_name, fallback)
	if entry is Object:
		var property_list: Array = entry.get_property_list()
		for prop in property_list:
			if str(prop.get("name", "")) == field_name:
				return entry.get(field_name)
	return fallback


func _find_entry(model: Variant, row_id: String) -> Variant:
	for entry in _extract_entries(model):
		if _extract_entry_id(entry) == row_id:
			return entry
	return null


func _collect_visible_row_ids(panel: Variant) -> Array[String]:
	var visible_ids: Array[String] = []
	if panel == null:
		return visible_ids

	var rows_container: Variant = panel.get("_status_rows")
	if rows_container == null:
		return visible_ids

	for child in rows_container.get_children():
		if child == null or not bool(child.visible):
			continue
		if child.has_method("get_entry_id"):
			visible_ids.append(str(child.call("get_entry_id")))
			continue
		var fallback_id := str(child.get("_entry_id"))
		if fallback_id != "":
			visible_ids.append(fallback_id)

	return visible_ids


func _classify_observed_outcome(
	contract_gaps: Array,
	projection_gaps: Array,
	row_ids: Array[String]
) -> String:
	if not contract_gaps.is_empty():
		return "contract_fallback"

	if not projection_gaps.is_empty():
		return "projection_gap"

	for row_id in row_ids:
		if row_id.begins_with("native_object/"):
			return "adaptive_projection"

	return "projected"

func _evaluate_expectations(
	expected_panel_outcome: Dictionary,
	snapshot_reading: Dictionary,
	contract_gaps: Array,
	projection_gaps: Array,
	row_ids: Array[String],
	visible_row_ids: Array[String],
	rendered_model: Variant
) -> Array[String]:
	var failures: Array[String] = []

	if expected_panel_outcome.has("snapshot_state_contains"):
		var needle := str(expected_panel_outcome.get("snapshot_state_contains", ""))
		var haystack := str(snapshot_reading.get("state", ""))
		if haystack.findn(needle) == -1:
			failures.append("snapshot_state missing substring '%s': %s" % [needle, haystack])

	if expected_panel_outcome.has("contract_gap_count"):
		var want_contract := int(expected_panel_outcome.get("contract_gap_count", 0))
		if contract_gaps.size() != want_contract:
			failures.append(
				"contract_gap_count mismatch: got=%d want=%d gaps=%s"
				% [contract_gaps.size(), want_contract, contract_gaps]
			)

	if expected_panel_outcome.has("projection_gap_count"):
		var want_projection := int(expected_panel_outcome.get("projection_gap_count", 0))
		if projection_gaps.size() != want_projection:
			failures.append(
				"projection_gap_count mismatch: got=%d want=%d gaps=%s"
				% [projection_gaps.size(), want_projection, projection_gaps]
			)

	if expected_panel_outcome.has("rendered_contract_gap_rows"):
		var want_rendered := int(expected_panel_outcome.get("rendered_contract_gap_rows", 0))

		var rendered_contract_gap_rows := 0
		for id in row_ids:
			if typeof(id) == TYPE_STRING and id.ends_with("/contract_gaps"):
				rendered_contract_gap_rows += 1

		if rendered_contract_gap_rows != want_rendered:
			failures.append(
				"rendered_contract_gap_rows mismatch: got=%d want=%d"
				% [rendered_contract_gap_rows, want_rendered]
			)

	var required_row_ids: Array = expected_panel_outcome.get("required_row_ids", [])
	for raw_id in required_row_ids:
		var row_id := str(raw_id)
		if not row_ids.has(row_id):
			failures.append("missing required row id: %s" % row_id)

	var forbidden_row_ids: Array = expected_panel_outcome.get("forbidden_row_ids", [])
	for raw_forbidden in forbidden_row_ids:
		var forbidden_id := str(raw_forbidden)
		if row_ids.has(forbidden_id):
			failures.append("forbidden row id present: %s" % forbidden_id)

	var required_visible_row_ids: Array = expected_panel_outcome.get("required_visible_row_ids", [])
	for raw_visible_id in required_visible_row_ids:
		var visible_id := str(raw_visible_id)
		if not visible_row_ids.has(visible_id):
			failures.append("missing required visible row id: %s" % visible_id)

	var forbidden_visible_row_ids: Array = expected_panel_outcome.get("forbidden_visible_row_ids", [])
	for raw_forbidden_visible in forbidden_visible_row_ids:
		var forbidden_visible_id := str(raw_forbidden_visible)
		if visible_row_ids.has(forbidden_visible_id):
			failures.append("forbidden visible row id present: %s" % forbidden_visible_id)

	var required_counters_by_row: Dictionary = expected_panel_outcome.get("required_counters_by_row", {})
	for row_id in required_counters_by_row.keys():
		var entry: Variant = _find_entry(rendered_model, str(row_id))
		if entry == null:
			failures.append("missing row for counter expectations: %s" % str(row_id))
			continue
		var counters: Dictionary = _extract_entry_counters(entry)
		var required_counter_names: Array = required_counters_by_row.get(row_id, []) as Array
		for raw_counter_name in required_counter_names:
			var counter_name := str(raw_counter_name)
			if not counters.has(counter_name):
				failures.append("row %s missing counter %s" % [str(row_id), counter_name])

	var required_counter_values_by_row: Dictionary = expected_panel_outcome.get("required_counter_values_by_row", {})
	for row_id in required_counter_values_by_row.keys():
		var entry: Variant = _find_entry(rendered_model, str(row_id))
		if entry == null:
			failures.append("missing row for counter value expectations: %s" % str(row_id))
			continue
		var counters: Dictionary = _extract_entry_counters(entry)
		var expected_values: Dictionary = required_counter_values_by_row.get(row_id, {}) as Dictionary
		for counter_name in expected_values.keys():
			var counter_key := str(counter_name)
			if not counters.has(counter_key):
				failures.append("row %s missing counter %s" % [str(row_id), counter_key])
				continue
			if int(counters[counter_key]) != int(expected_values[counter_name]):
				failures.append(
					"row %s counter %s mismatch: got=%d want=%d"
					% [str(row_id), counter_key, int(counters[counter_key]), int(expected_values[counter_name])]
				)

	var required_badges_by_row: Dictionary = expected_panel_outcome.get("required_badges_by_row", {})
	for row_id in required_badges_by_row.keys():
		var entry: Variant = _find_entry(rendered_model, str(row_id))
		if entry == null:
			failures.append("missing row for badge expectations: %s" % str(row_id))
			continue
		var badges: Array[String] = _extract_entry_badges(entry)
		var required_badges: Array = required_badges_by_row.get(row_id, []) as Array
		for raw_badge in required_badges:
			var badge := str(raw_badge)
			if not badges.has(badge):
				failures.append("row %s missing badge %s" % [str(row_id), badge])

	var required_info_substrings_by_row: Dictionary = expected_panel_outcome.get("required_info_substrings_by_row", {})
	for row_id in required_info_substrings_by_row.keys():
		var entry: Variant = _find_entry(rendered_model, str(row_id))
		if entry == null:
			failures.append("missing row for info expectations: %s" % str(row_id))
			continue
		var info_lines: Array[String] = _extract_entry_info_lines(entry)
		var info_blob: String = "\n".join(info_lines)
		var required_substrings: Array = required_info_substrings_by_row.get(row_id, []) as Array
		for raw_substring in required_substrings:
			var needle := str(raw_substring)
			if info_blob.find(needle) == -1:
				failures.append("row %s missing info substring %s" % [str(row_id), needle])

	return failures


func _printerr(message: String) -> void:
	push_error(message)
	printerr(message)
