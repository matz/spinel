# A top-level def made public answers every reader of Object's methods as
# CRuby does: a call on a value that is held boxed (an Object, an element of
# a mixed Array), public_send on main, and respond_to? on main, on a
# builtin value, on a boxed one and on nil. Private it stays private: the
# calls raise, and respond_to? is true only with include_all.

def pa = :pa
def pb(x) = [:pb, x]
p((Object.new.pa rescue $!.message))
p respond_to?(:pa), respond_to?(:pa, true), 3.respond_to?(:pa), 3.respond_to?(:pa, true)
p((public_send(:pa) rescue $!.message))

public :pa, :pb
p Object.new.pa, Object.new.pb(1)
p respond_to?(:pa), respond_to?(:pa, true), 3.respond_to?(:pa), "s".respond_to?(:pb), nil.respond_to?(:pa)
p public_send(:pa), public_send(:pb, 2), 3.public_send(:pa)

mixed = [1, "s", nil, :sym, 2.5, [1], { a: 1 }, true, Object.new]
p mixed.map { |x| x.pa }
p mixed.map { |x| x.pb(0) }.first
p mixed.map { |x| x.respond_to?(:pa) }.all?

private :pa
p((Object.new.pa rescue $!.message))
p respond_to?(:pa), respond_to?(:pa, true), 3.respond_to?(:pa), 3.respond_to?(:pa, true), nil.respond_to?(:pa, true)
p((public_send(:pa) rescue $!.message))
p((mixed.map { |x| x.pa } rescue $!.message))
