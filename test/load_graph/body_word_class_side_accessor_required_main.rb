# A class method's `private_class_method :tok` names a class-side reader a
# required file made and a `def self.tok` that follows the require replaced:
# the def is the method the name means (the load order, not the tree's, says
# which came last), so the word makes it private as for any class method.
require_relative "body_word_class_side_accessor_required_lib"
class K
  def self.tok = :defd
  def self.hide
    private_class_method :tok
    nil
  end
end
p [K.tok]
K.hide
p [(K.tok rescue $!.class), K.respond_to?(:tok), K.send(:tok)]
