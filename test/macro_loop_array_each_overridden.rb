# a program's own Array#each that does not yield: the macro's loop defines
# nothing, as in CRuby (the runner records each as a method it ran)
class Array
  def each = self
end
class K
  def self.mk = [:field].each { |n| attr_accessor n }
  mk
end
p K.new.respond_to?(:field)
