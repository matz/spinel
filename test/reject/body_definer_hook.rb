# A method_added or singleton_method_added hook of the program observes the method a body's definer makes where
# the body runs; the method is not defined at that point, so no hook call is made there. CRuby prints "before",
# "added late_label", 2, "added late_tag", 3.
class H
  def self.method_added(n) = (puts "added #{n}" if n == :late_label)
  def self.singleton_method_added(n) = (puts "added #{n}" if n == :late_tag)
  def self.install = (define_method(:late_label) { 2 }; define_singleton_method(:late_tag) { 3 }; :ok)
end
puts "before"
H.install
p H.new.late_label
p H.late_tag
