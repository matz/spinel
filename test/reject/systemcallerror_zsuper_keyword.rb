# A bare super in an initialize with a declared keyword hands
# SystemCallError#initialize a Hash of keywords (a TypeError in CRuby);
# passing the keyword's value by position would build an exception instead.
class E < Errno::ENOENT
  def initialize(msg:)
    super
  end
end
p E.new(msg: "x").message
