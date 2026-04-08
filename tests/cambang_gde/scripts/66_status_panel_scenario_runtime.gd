extends Control

const BUILTIN_SCENARIO := "stream_lifecycle_versions"

var _connected := false


func _ready() -> void:
	if not Engine.has_singleton("CamBANGServer"):
		push_warning("CamBANGServer singleton not available.")
		return
		
	await get_tree().create_timer(10.0).timeout

	CamBANGServer.stop()

	var start_err := CamBANGServer.start(
		CamBANGServer.PROVIDER_KIND_SYNTHETIC,
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME
	)
	if start_err != OK:
		push_error("FAIL: synthetic timeline start rejected with error %d" % start_err)
		return

	var select_err := CamBANGServer.select_builtin_scenario(BUILTIN_SCENARIO)
	if select_err != OK:
		push_error("FAIL: unable to select builtin scenario %s" % BUILTIN_SCENARIO)
		return

	var enable_err := CamBANGServer.set_completion_gated_destructive_sequencing_enabled(true)
	#var enable_err := CamBANGServer.set_completion_gated_destructive_sequencing_enabled(false)
	if enable_err != OK:
		push_error("FAIL: unable to set completion-gated destructive sequencing")
		return

	var scenario_start_err := CamBANGServer.start_scenario()
	if scenario_start_err != OK:
		push_error("FAIL: unable to start builtin scenario %s" % BUILTIN_SCENARIO)
		return

	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)
		_connected = true

	print("RUN: status panel scenario runtime (server-driven) scenario=%s" % BUILTIN_SCENARIO)


func _exit_tree() -> void:
	if not Engine.has_singleton("CamBANGServer"):
		return

	if _connected and CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
		_connected = false

	CamBANGServer.stop()


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		print("INFO: publish gen=%d version=%d topology=%d snapshot=nil" % [gen, version, topology_version])
		return

	var device_count := int((snapshot.get("devices", []) as Array).size())
	var stream_count := int((snapshot.get("streams", []) as Array).size())
	print("INFO: publish gen=%d version=%d topology=%d devices=%d streams=%d" % [gen, version, topology_version, device_count, stream_count])
