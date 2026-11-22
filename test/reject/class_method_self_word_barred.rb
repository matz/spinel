# A visibility word sent to an explicit `self.` (`self.send(:private, def a)`)
# in a class method where the record applies a word from the start (here a
# body that yields, whose block it cannot follow) is refused at compile time,
# as it is for the form with a receiver other than self; a Module's word would
# otherwise be sent to an instance.
class J
  def self.setup
    self.send(:private, def a = :a)
    yield
  end
end
J.setup { 1 }
p((J.new.a rescue $!.class))
