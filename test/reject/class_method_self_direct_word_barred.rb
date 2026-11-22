# An explicit `self.` visibility word written directly (`self.private def b`)
# in a class method where the record applies a word from the start (here a
# body that yields, whose block it cannot follow) is refused at compile time,
# as it is for the form with a receiver other than self; a Module's word would
# otherwise be sent to an instance.
class J
  def self.setup
    self.private def b = :b
    yield
  end
end
J.setup { 2 }
p((J.new.b rescue $!.class))
