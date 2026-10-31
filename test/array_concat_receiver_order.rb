$log = []
class Source
  def initialize; $log << :argument; end
  def to_ary; $log << :conversion; [2]; end
end
def receiver
  $log << :receiver
  [1]
end
receiver.concat(Source.new)
p $log
$log = []
p receiver.concat(Source.new)
p $log
