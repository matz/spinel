# A forward's keyword parameters, passed on by its bare `super` as
# keywords, ride through a later forward with a keyword rest and no
# keywords of its own to Hash's initialize: their defaults run after the
# positional ones, the call's keywords merged over them.
$log = []
module Inner
  def initialize(*a, **kw) = super
end
module Outer
  def initialize(d = ($log << :d; nil), capacity: ($log << :c; 0)) = super
end
Hash.prepend(Inner)
Hash.prepend(Outer)
p Hash.new[:x], $log
$log.clear
p Hash.new(3)[:x], $log
$log.clear
p Hash.new(4, capacity: ($log << :given; 2))[:x], $log
