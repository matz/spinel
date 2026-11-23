# The operands of a visibility word run in order: a def after an operand that raises is never made, one before it is.
class K
  def a = :a
  begin; private raise("x"), def x = 1; rescue RuntimeError; end
  begin; private def y = 2, raise("x"), def z = 3; rescue RuntimeError; end
  begin; protected raise("x"), def x2 = 1; rescue RuntimeError; end
  begin; private_class_method raise("x"), def self.cx = 1; rescue RuntimeError; end
  begin; private fail("z"), def w = 4; rescue RuntimeError; end
end
module M
  begin; module_function raise("stop"), def g1 = 1, def g2 = 2; rescue RuntimeError; end
end
class H; include M; end
k = K.new
p [(k.send(:x) rescue $!.class), k.y, (k.send(:z) rescue $!.class), (k.send(:x2) rescue $!.class), (K.send(:cx) rescue $!.class), (k.send(:w) rescue $!.class)]
p [(H.new.send(:g1) rescue $!.class), (H.new.send(:g2) rescue $!.class), M.respond_to?(:g1)]
class K2
  begin; private(def before; :ok; end, raise("stop")); rescue RuntimeError; end
  begin; private(raise("stop"), (def z2; :ok; end)); rescue RuntimeError; end
end
k2 = K2.new
p [k2.before, (k2.send(:z2) rescue $!.class)]
