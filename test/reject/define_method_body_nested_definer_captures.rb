# A definer inside a define_singleton_method's block (a def around both) whose own
# block reads that block's parameter is refused where the program calls the
# name it defines: the parameter is a local of the outer block, which has no
# cell, and the inner method would read nil.
class K
  def self.install
    define_singleton_method(:factory) { |value| define_method(:read) { value }; :ok }
  end
end
K.install
K.factory(5)
p K.new.read
