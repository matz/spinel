# A reopened StandardError#initialize that takes no argument runs for a KeyError built with keywords only:
# CRuby's own initialize takes the keywords off and calls super with none, so no ArgumentError.
class StandardError
  def initialize() = super()
end
e = KeyError.new(key: 2)
p e.message
p e.key
f = FrozenError.new(receiver: 3)
p f.message
p f.receiver
