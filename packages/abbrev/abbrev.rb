# Spinel package: abbrev
#
# Abbrev.abbrev(words, pattern = nil) and Array#abbrev: a Hash from every
# unambiguous prefix of the words, and each word itself, to its word. A
# String pattern keeps the prefixes that start with it, a Regexp the ones
# it matches.

module Abbrev
  VERSION = "0.1.2"

  def abbrev(words, pattern = nil)
    pattern = /\A#{Regexp.quote(pattern)}/ if pattern.is_a?(String)
    owners = {}
    words.each do |word|
      word.size.downto(1) do |len|
        prefix = word[0, len]
        next if pattern && pattern !~ prefix
        owners[prefix] = owners.key?(prefix) ? nil : word
      end
    end
    table = owners.compact
    words.each { |word| table[word] = word unless pattern && pattern !~ word }
    table
  end

  module_function :abbrev
end

class Array
  def abbrev(pattern = nil)
    Abbrev.abbrev(self, pattern)
  end
end
