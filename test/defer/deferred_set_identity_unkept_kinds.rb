# Compiled with --defer-refusals: an identity Set's member whose kind the
# compiler cannot tell is checked when it arrives, and a value whose
# identity Spinel does not keep raises NotImplementedError there, in the
# words of the refusal: a Float that is no flonum in CRuby (1e300), an
# Integer past CRuby's fixnum range, a Rational, a Regexp literal. The values
# beside them pass (a String Symbol#to_s answers among them: a new String at
# each call, as CRuby's), and a refused one does not join the Set. Without
# the flag the line is refused at compile time.
# spinel: defer-refusals: 3:refused ok ok refused refused ok refused ok ok 5 false
require "set"

class Item; end

def pick(list, i) = list[i]

def try(seen, v)
  seen << v
  "ok"
rescue NotImplementedError => e
  e.message.include?("an identity Set would find it otherwise than CRuby's") ? "refused" : e.message
end

seen = Set.new.compare_by_identity
list = [1e300, Item.new, 2**62 - 1, 2**62, Rational(1, 3), :name.to_s, /re/, Regexp.new("re"), 2.5]
list.size.times { |i| puts try(seen, pick(list, i)) }
puts seen.size
puts seen.include?(pick([Item.new, 2.5], 0))
proc { "a" }.ruby2_keywords
puts "not reached"
