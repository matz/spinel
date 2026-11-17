class K
  attr_reader :v
  def initialize(x) = @v = x
end
k = [K, String][ARGV.size]
a = [0, (class K; def self.new(x) = super(x * 7); end; 0)]
p a
p K.new(4).v
p k.new(5).v
