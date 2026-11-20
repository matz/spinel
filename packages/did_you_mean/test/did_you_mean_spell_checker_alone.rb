# require "did_you_mean/spell_checker" alone, without did_you_mean itself:
# the file requires the two distances it measures with, as CRuby's does, so
# SpellChecker#correct answers.
require "did_you_mean/spell_checker"

def correct(input, dictionary) = p DidYouMean::SpellChecker.new(dictionary: dictionary).correct(input)

correct("doo", %w[foo fork])
correct("meail", %w[email fail eval])
correct("verbos", [:verbose, :version, "help"])
correct("nothing", %w[alpha beta])
