# A def in a class_eval block of the body's own class fires method_added
# where it stands, between the block's other statements.
class A
  def self.method_added(n) = puts("added #{n}")
  class_eval do
    def one = 1
    puts "between"
    def two = 2
  end
end
