class A
  class << self
    private
    def sp = 1
  end
end
k = [A][ARGV.size]
n = [:sp][ARGV.size]
p k.respond_to?(n)
p(k.respond_to?(n) ? :yes : :no)
