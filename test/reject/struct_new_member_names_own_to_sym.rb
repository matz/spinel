# Struct.new(*%w[a b].map(&:to_sym)) where the program reopens String#to_sym:
# the names come from the program's own method, which only runs at run time
# (CRuby gives [:ax, :bx]), so it is refused rather than given the literal's
# names [:a, :b]. A to_sym of another class leaves the list readable.
class String
  def to_sym = :"#{self}x"
end
R = Struct.new(*%w[a b].map(&:to_sym))
p R.members
