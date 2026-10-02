# A class reopened through a constant alias: the body's cref is the aliased
# class, so its own constants are in scope. The bare-constant lookup model
# does not follow the alias and must leave the program alone, not refuse it.
module Shapes
  class Polygon
    CORNERS = 4
  end
end
PolygonAlias = Shapes::Polygon
class PolygonAlias
  def corners = CORNERS
end
p Shapes::Polygon.new.corners

PolygonAlias::EDGES = 5
class Shapes::Polygon
  def edges = EDGES
end
p Shapes::Polygon.new.edges
