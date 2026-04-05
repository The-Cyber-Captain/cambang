extends Control

func _ready() -> void:
	if not Engine.has_singleton("CamBANGServer"):
		push_warning("CamBANGServer singleton not available.")
		return
	CamBANGServer.stop()
	CamBANGServer.start_synthetic_with_role_and_timing(
		CamBANGServer.SYNTHETIC_ROLE_TIMELINE,
		CamBANGServer.TIMING_DRIVER_VIRTUAL_TIME
	)


func _exit_tree() -> void:
	if Engine.has_singleton("CamBANGServer"):
		CamBANGServer.stop()
