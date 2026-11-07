# The table of unambiguous abbreviations, through Abbrev.abbrev, Array#abbrev
# and Abbrev mixed into a class, with ruby/abbrev's own cases, and the
# order of the Hash it answers.
require "abbrev"

def show(table)
  table.sort.each { |k, v| p [k, v] }
  puts "--"
end

words = %w[summer winter win ruby rules]
show(words.abbrev)
show(Abbrev.abbrev(words))
p words.abbrev == Abbrev.abbrev(words)
show(["abc", "abc\nd", "de"].abbrev)

show(%w[car cone].abbrev)
p %w[car cone cart same same].abbrev
show(%w[a ab abc].abbrev)
show(%w[same same other].abbrev)
show(["", "x"].abbrev)
p [].abbrev
show(%w[ação água].abbrev)

class Completer
  include Abbrev

  def initialize(words)
    @words = words
  end

  def complete(prefix)
    abbrev(@words)[prefix]
  end
end

c = Completer.new(%w[status stash start])
p c.complete("stat"), c.complete("sta"), c.complete("stas"), c.complete("start")
p Abbrev::VERSION
