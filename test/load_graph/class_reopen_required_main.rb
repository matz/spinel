require_relative "class_reopen_required_lib"

class Base; end
class Mid < Base; end
class Leaf < Mid; end

p Base.descendants.map(&:name).sort
p Leaf.descendants
