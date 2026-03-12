extends Control

func _ready() -> void:
	if not Engine.has_singleton("CamBANGServer"):
		push_warning("CamBANGServer singleton not available.")
		return
	CamBANGServer.stop()
	CamBANGServer.set_provider_mode("synthetic")
	CamBANGServer.start()


func _exit_tree() -> void:
	if Engine.has_singleton("CamBANGServer"):
		CamBANGServer.stop()
