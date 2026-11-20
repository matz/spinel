# A list of a builtin class's own instance methods on a constant, where the
# program reopens the class and a class method's body defines one, reads
# that def's version cells as a list on a class of the program does: the
# name is listed, and answers respond_to?, once the body has run.
class Object
  def self.setup_zz = (def zz = :zz)
end
class K; end
p [Object.instance_methods(false).include?(:zz), Object.public_instance_methods(false).include?(:zz),
   1.respond_to?(:zz), K.new.respond_to?(:zz), (1.zz rescue $!.class)]
Object.setup_zz
p [Object.instance_methods(false).include?(:zz), Object.public_instance_methods(false).include?(:zz),
   1.respond_to?(:zz), K.new.respond_to?(:zz), 1.zz, K.new.zz]
