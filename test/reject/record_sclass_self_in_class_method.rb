# The same through `class << self` in a Struct class's own class method:
# CRuby prints "false" before S.setup runs and "true" after.
S = Struct.new(:v) do
  def self.setup
    class << self
      def k = :mk
    end
  end
end
p S.respond_to?(:k)
S.setup
p S.respond_to?(:k)
