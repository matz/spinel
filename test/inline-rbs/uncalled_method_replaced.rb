# A later def without an annotation replaces an annotated module method
# nothing calls, and another def follows: the compile warns that the
# signature is not applied and names the replacement's line.
module Names
  #: (String) -> String
  def self.lookup(key) = key

  def self.lookup(key) = key

  def self.key(name) = name
end
puts "unused"
