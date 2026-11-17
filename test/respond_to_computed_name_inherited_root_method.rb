# A class that inherits an Object method answers it only once the reopening that defines it has run.
class Child < Object; end
class Plain; end
name = ARGV[0] ? "other" : "later"
puts Object.new.respond_to?(name, true)
puts Child.new.respond_to?(name, true)
puts Plain.new.respond_to?(name, true)
class Object
  def later = 1
end
puts Object.new.respond_to?(name, true)
puts Child.new.respond_to?(name, true)
puts Plain.new.respond_to?(name, true)
