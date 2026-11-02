# A `when Klass` or `in Klass` arm calls the class's own class method `===`
# with the case subject, so the subject is that call's argument: a store
# through the parameter that its array cannot hold widens the subject's
# array where it is built, as a written `Klass === a` does. A boxed subject
# is followed back to its arrays the same way.
class Tagger
  def self.===(x)
    x << "s" if x.is_a?(Array)
    true
  end
end

a = [1]
case a
when Tagger then p a
end

b = [2]
case b
in Tagger then p b
end

c = [3]
p Tagger === c
p c

[[4], "d"].each do |v|
  case v
  when Tagger then p v
  end
end

# A class method `===` with a keyword is not the one the arms call: a
# written call reaches it, and asks for its keyword.
class Keyed
  def self.===(value:) = value
end
p((Keyed === 5 rescue :argument_error))
p Keyed.===(value: 7)
