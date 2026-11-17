# Through a prepended forward with keyword parameters and no keyword rest,
# a `**h` beside a literal key is checked as that forward binds; a later
# forward that does not name a key the first passes on raises after the
# first's defaults run. The call would need both checks in CRuby's order,
# which the builtin construction does not make, so it is refused, not
# answered with the later one's keys.
$l = []
module Later; def initialize(*a, foo: 1) = super; end
module Check; def initialize(d = ($l << :check; 3), capacity: 1) = super; end
Hash.prepend(Later); Hash.prepend(Check)
p((Hash.new(capacity: 2, **{ bar: 1 }) rescue $!.message), $l)
p((Hash.new(capacity: 2, **{}) rescue $!.message), $l)
