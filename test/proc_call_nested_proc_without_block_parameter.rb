def inner(&callback)
  callback.call { 1 }
end
def outer(&callback)
  callback.call { inner { |&blk| blk.call } }
end
puts outer(&proc { "kept" })
