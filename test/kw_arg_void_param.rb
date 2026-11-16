# A constant filled by a method that always raises (a stub left in place of
# a method the program never meant to run) is void to the analysis. Passed as
# a keyword argument, it bound the parameter void too, and codegen refused
# the method ("param 'block' has unsupported type void") where the same
# constant as a positional argument bound the parameter poly. The keyword
# binds as the positional does: the parameter holds any value, and the
# argument raises when the call is reached, which here ends the program the
# way CRuby ends it, in the class body that fills the constant.
def missing_default = raise(NotImplementedError, "no default here")

def pick(versions:, block: nil) = "#{versions}: #{block.inspect}"

def sink(a:) = a.inspect

def plain(a) = a.inspect

puts "start"

class Browsers
  DEFAULT_RESPONSE = missing_default

  def self.allow(versions:, block: nil)
    pick(versions: versions, block: block || DEFAULT_RESPONSE)
  end

  def self.keyword = sink(a: DEFAULT_RESPONSE)

  def self.positional = plain(DEFAULT_RESPONSE)
end

puts Browsers.allow(versions: :modern)
puts Browsers.keyword
puts Browsers.positional
