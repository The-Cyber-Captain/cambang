extends SceneTree

const STATUS_PANEL_SCRIPT := preload("res://addons/cambang/cambang_status_panel.gd")
const DEFAULT_VIEWPORT_SIZE := Vector2i(1280, 720)
const SnapshotValidator = preload("res://support/snapshot_schema_validator.gd")

func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 1:
		_printerr("usage: -- <fixture.json> [screenshot.png]")
		quit(2)
		return

	var fixture_path := args[0]
	var screenshot_path := args[1] if args.size() >= 2 else ""

	var fixture := _load_fixture(fixture_path)
	if fixture.is_empty():
		quit(2)
		return

	var payload: Variant = fixture.get("payload", null)
	var schema_errors: Array[String] = []

	if typeof(payload) == TYPE_DICTIONARY:
		schema_errors = SnapshotValidator.validate_snapshot(payload)
	else:
		schema_errors.append("payload must be Dictionary")

	print("HARNESS schema_errors: %s" % [schema_errors])

	var expected_validity := str(fixture.get("expected_validity", "valid"))
	var should_run_projector := bool(fixture.get("should_run_projector", true))
	var expected_panel_outcome: Dictionary = fixture.get("expected_panel_outcome", {})
	var provider_mode := str(fixture.get("provider_mode", "fixture"))

	var window := Window.new()
	window.title = "status_panel_harness"
	window.size = DEFAULT_VIEWPORT_SIZE
	window.mode = Window.MODE_WINDOWED
	window.visible = true
	get_root().add_child(window)

	var panel: PanelContainer = STATUS_PANEL_SCRIPT.new()
	panel.name = "CamBANGStatusPanel"
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	window.add_child(panel)

	await process_frame
	panel.call("_disconnect_server")

	var compat: Dictionary = _compute_runtime_compat(panel, payload)
	var contract_gaps: Array = compat.get("contract_gaps", [])
	var projection_gaps: Array = compat.get("projection_gaps", [])
	var is_schema_valid := schema_errors.is_empty()
	var is_projection_compatible := projection_gaps.is_empty()

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
	var snapshot_reading: Dictionary = panel.call("_read_snapshot", payload)

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

	panel.call("_apply_snapshot_read", snapshot_reading)
	panel.call("_render_panel_model", rendered_model)

	await process_frame
	await process_frame

	if screenshot_path != "":
		_capture_window_png(window, screenshot_path)

	var row_ids := _collect_entry_ids(rendered_model)
	print("HARNESS row_ids: %s" % [row_ids])
	_dump_rendered_entries(rendered_model)

	var failures := _evaluate_expectations(
		expected_panel_outcome,
		snapshot_reading,
		contract_gaps,
		projection_gaps,
		row_ids
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


func _capture_window_png(window: Window, screenshot_path: String) -> void:
	var texture := window.get_texture()
	if texture == null:
		push_warning("screenshot skipped: window texture unavailable in current renderer/headless mode")
		return

	var image: Image = texture.get_image()
	if image == null:
		push_warning("screenshot skipped: failed to read image from window texture")
		return

	var err := image.save_png(screenshot_path)
	if err != OK:
		_printerr("failed to save screenshot: %s (err=%d)" % [screenshot_path, err])


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
		var detail := _extract_entry_field(entry, "detail")
		var parent_id := _extract_entry_field(entry, "parent_id")
		print("  id=%s parent=%s label=%s detail=%s" % [entry_id, parent_id, label, detail])


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
	row_ids: Array[String]
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

	return failures


func _printerr(message: String) -> void:
	push_error(message)
	printerr(message)
