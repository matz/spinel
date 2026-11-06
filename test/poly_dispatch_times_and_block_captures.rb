# A boxed Integer's times, in a dispatch that exists because a class of the
# program defines `times` too, runs the block over 0...n and answers the
# Integer; and the blocks the dispatch's builtin arms drive as a proc
# (times, each_index, synchronize) write the enclosing method's locals.
class Options
  def initialize(h) = @h = h
  def times = @h[:times]
  def each_index = @h[:each_index]
  def synchronize = @h[:synchronize]
end

class Server
  def initialize(config)
    @config = config
    @tokens = []
    @config[:max_clients].times { @tokens.push(nil) }
    p @tokens.size
    sum = 0
    r = @config[:max_clients].times { |i| sum += i }
    p [sum, r]
    seen = []
    @config[:list].each_index { |i| seen << i * 10 }
    total = 0
    @config[:list].each_index { |i| total = total + @config[:list][i] }
    p [seen, total]
    n = 0
    @config[:lock].synchronize { n += 1 }
    p n
  end
end

Server.new({ max_clients: 4, list: [5, 6, 7], lock: Mutex.new, name: "web" })
o = Options.new({ times: 1, each_index: 2, synchronize: 3 })
p [o.times, o.each_index, o.synchronize]
