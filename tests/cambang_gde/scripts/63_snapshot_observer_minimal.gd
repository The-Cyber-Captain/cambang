extends Node

var _line_count := 0


func _ready() -> void:
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")
	if not CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.connect(_on_state_published)
	CamBANGServer.start()


func _exit_tree() -> void:
	if CamBANGServer.state_published.is_connected(_on_state_published):
		CamBANGServer.state_published.disconnect(_on_state_published)
	CamBANGServer.stop()


func _on_state_published(gen: int, version: int, topology_version: int) -> void:
	var snapshot = CamBANGServer.get_state_snapshot()
	if snapshot == null:
		return
	var streams: Array = snapshot.get("streams", [])
	var frames_received := 0
	var frames_delivered := 0
	var frames_dropped := 0
	var stream_errors := 0
	for stream in streams:
		frames_received += int(stream.get("frames_received", 0))
		frames_delivered += int(stream.get("frames_delivered", 0))
		frames_dropped += int(stream.get("frames_dropped", 0))
		if int(stream.get("stop_reason", 0)) != 0:
			stream_errors += 1

	print(
		"OBS: gen=", gen,
		" version=", version,
		" topology_version=", topology_version,
		" devices=", int((snapshot.get("devices", []) as Array).size()),
		" streams=", streams.size(),
		" frames_received=", frames_received,
		" frames_delivered=", frames_delivered,
		" frames_dropped=", frames_dropped,
		" stream_errors=", stream_errors
	)

	_line_count += 1
	if _line_count >= 5:
		print("OK: godot snapshot observer minimal PASS")
		call_deferred("_quit_next_frame")


func _quit_next_frame() -> void:
	await get_tree().process_frame
	get_tree().quit(0)
