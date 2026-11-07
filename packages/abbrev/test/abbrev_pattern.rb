# The pattern argument: a String is a prefix, taken literally, and a Regexp
# filters both the abbreviations and the full words.
require "abbrev"

def show(table)
  table.sort.each { |k, v| p [k, v] }
  puts "--"
end

words = %w[summer winter win ruby rules]
show(words.abbrev("ru"))
show(Abbrev.abbrev(words, "ru"))
show(["abc", "abc\nd", "de"].abbrev("d"))
show(words.abbrev("w"))
show(words.abbrev("zzz"))
show(words.abbrev(""))

show(%w[a.b axb a.c].abbrev("a."))
show(%w[a.b axb a.c].abbrev(/\Aa./))
show(words.abbrev(/r/))
show(words.abbrev(/er\z/))
show(%w[Ruby rust RUBY].abbrev(/\Ar/i))
