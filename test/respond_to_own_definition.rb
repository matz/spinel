# A class's own respond_to? answers for itself: respond_to_missing? is not
# consulted behind it.
class Shy
  def respond_to?(name, all = false) = false
  def respond_to_missing?(name, all = false) = true
  def method_missing(name, *args) = :caught
end
s = Shy.new
p s.respond_to?(:anything), s.anything
