# A module_function in a begin body with rescue clauses, or in a block there,
# changes each name before the one it raises for, as CRuby's word does: those
# names are private, and none is copied to the module, as the word makes
# every name private before it copies any. In a block that runs no time, or
# after a `next`, the word does not run and changes nothing.
module M
  def a = :a
  def b = :b
  def c = :c
  begin
    module_function :a, "b", :zz
  rescue NameError => e
    p e.message
  end
  begin
    module_function ["c", 1]
  rescue TypeError => e
    p e.message
  end
end
class K; include M; end
p [(K.new.a rescue $!.class), (K.new.b rescue $!.class), (K.new.c rescue $!.class)]
p [M.respond_to?(:a), M.respond_to?(:b), M.respond_to?(:c)]
module MB
  def a = :a
  begin
    [1].each { module_function :a, :zz }
  rescue NameError => e
    p e.class
  end
end
class KB; include MB; end
p [(KB.new.a rescue $!.class), MB.respond_to?(:a)]
module MZ
  def x = :x
  def y = :y
  begin
    [].each { module_function :x, :zz }
    [1].each do
      next
      module_function :y, :zz
    end
  rescue NameError => e
    p e.class
  end
end
class KZ; include MZ; end
p [(KZ.new.x rescue $!.class), (KZ.new.y rescue $!.class), MZ.respond_to?(:x), MZ.respond_to?(:y)]
