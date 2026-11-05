# A Method of an alias of a class's accessor stands where the accessor it
# captured is declared, and is owned by the class whose body holds the
# alias, as CRuby answers; one of a Struct's member stands nowhere.
class C
  attr_reader :x
  alias y x
  alias_method :z, :y
end
S = Struct.new(:a)
class S
  alias b a
end

def show(s) = p(s ? [File.basename(s[0]), s[1]] : s)

o = C.new
show o.method(:y).source_location
show o.method(:z).source_location
p o.method(:y).owner, o.method(:z).owner
m = o.method(:y)
m = o.method(:z) if o.x.nil?
show m.source_location
p m.owner
show S.new(1).method(:b).source_location
