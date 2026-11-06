# A macro body that does more than define methods: the statement the form
# does not follow (a puts) runs where the call stands, before the method it
# defines.
class Base
  def self.mk(n)
    puts "making #{n}"
    define_method(n) { 1 }
  end
end
class Foo < Base
  mk :a
end
p Foo.new.a
