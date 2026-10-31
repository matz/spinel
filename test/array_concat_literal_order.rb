$log = []
def receiver
  $log << :receiver
  [1]
end
def argument
  $log << :argument
  2
end
receiver.concat([argument])
p $log
$log = []
p receiver.concat([argument])
p $log
