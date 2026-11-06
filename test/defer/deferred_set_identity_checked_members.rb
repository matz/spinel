# Compiled with --defer-refusals: a value an identity Set takes or is asked
# about, whose kind the compiler cannot tell (it is read out of an Array of
# mixed values), is checked when it arrives instead of refusing the line.
# An object, a Symbol, nil, an Integer in CRuby's fixnum range, a flonum
# Float and a String literal keep their identity here as in CRuby, so the
# check passes them and the Set finds them as CRuby's does. Without the
# flag these lines are refused at compile time.
# spinel: defer-refusals: 6:7 true false true false 6
require "set"

class Item; end

def pick(list, i) = list[i]

a = Item.new
b = Item.new
list = [a, :sym, nil, 7, 1.5, "lit", b]
seen = Set.new.compare_by_identity
list.size.times { |i| seen << pick(list, i) }
puts seen.size
puts list.all? { |v| seen.include?(v) }
puts seen.include?(pick([a, Item.new], 1))
puts seen.include?(pick([a, "lit"], 1))
seen.delete(pick(list, 0))
puts seen.include?(a)
puts seen.size
proc { "a" }.ruby2_keywords
puts "not reached"
