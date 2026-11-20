# Requires the two distances itself, as CRuby's does: `require
# "did_you_mean/spell_checker"` alone gives a working SpellChecker.
require_relative "levenshtein"
require_relative "jaro_winkler"

module DidYouMean
  # The words of a dictionary close to a given word, the closest first.
  # Words may be Strings or Symbols; the given word itself is never one of
  # them.
  class SpellChecker
    # How alike, by Jaro-Winkler, a word must be to be suggested. A word of
    # up to SHORT_WORD characters needs less: one character weighs more in it.
    ALIKE       = 0.834
    ALIKE_SHORT = 0.77
    SHORT_WORD  = 3

    # A mistype edits at most this share of the word's characters.
    MISTYPE_SHARE = 0.25

    def initialize(dictionary:)
      @dictionary = dictionary
    end

    def correct(input)
      target = normalize(input)
      words = alike(input, target)
      mistypes = words.select { mistype?(it, target) }
      mistypes.empty? ? first_misspelling(words, target) : mistypes
    end

    private

    # The words close enough by Jaro-Winkler, the most alike first.
    def alike(input, target)
      @dictionary
        .select { JaroWinkler.distance(normalize(it), target) >= threshold(target) }
        .reject { it.to_s == input.to_s }
        .sort_by { JaroWinkler.distance(it.to_s, target) }
        .reverse
    end

    def threshold(target) = target.length > SHORT_WORD ? ALIKE : ALIKE_SHORT

    def mistype?(word, target)
      Levenshtein.distance(normalize(word), target) <= (target.length * MISTYPE_SHARE).ceil
    end

    # The most alike word that is a misspelling, alone in a list, or none.
    def first_misspelling(words, target)
      words.select { misspell?(it, target) }.first(1)
    end

    # Fewer edits than the shorter of the two has characters.
    def misspell?(word, target)
      word = normalize(word)
      Levenshtein.distance(word, target) < [word.length, target.length].min
    end

    # Case does not count, nor the @ of an instance variable's name.
    def normalize(word) = word.to_s.downcase.delete("@")
  end
end
