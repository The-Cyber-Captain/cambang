@tool
extends Control

var _expanded: bool = false


func _ready() -> void:
	custom_minimum_size = Vector2(14, 14)
	queue_redraw()


func set_expanded(expanded: bool) -> void:
	if _expanded == expanded:
		return
	_expanded = expanded
	queue_redraw()


func _draw() -> void:
	var rect := Rect2(Vector2.ZERO, size)
	var center := rect.get_center()
	var radius := minf(rect.size.x, rect.size.y) * 0.4
	var points := PackedVector2Array()

	if _expanded:
		points.append(center + Vector2(-radius, -radius * 0.45))
		points.append(center + Vector2(radius, -radius * 0.45))
		points.append(center + Vector2(0.0, radius))
	else:
		var rtl := is_layout_rtl()
		if rtl:
			points.append(center + Vector2(radius * 0.85, -radius))
			points.append(center + Vector2(radius * 0.85, radius))
			points.append(center + Vector2(-radius, 0.0))
		else:
			points.append(center + Vector2(-radius * 0.85, -radius))
			points.append(center + Vector2(-radius * 0.85, radius))
			points.append(center + Vector2(radius, 0.0))

	draw_colored_polygon(points, Color(0.86, 0.88, 0.92, 1.0))
