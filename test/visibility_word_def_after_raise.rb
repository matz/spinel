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
p [(k.x rescue $!.class), (k.send(:y) rescue $!.class), (k.z rescue $!.class), (k.x2 rescue $!.class), (K.cx rescue $!.class), (k.w rescue $!.class)]
p [(H.new.g1 rescue $!.class), (H.new.g2 rescue $!.class), M.respond_to?(:g1)]
