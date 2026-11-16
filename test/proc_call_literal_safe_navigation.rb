def statement(&callback)
  callback&.call(puts("argument")) { "kept" }
  "done"
end

def expression(&callback)
  callback&.call(puts("argument")) { "kept" }
end

def ignored(&callback)
  callback&.call(puts("ignored argument")) { "dropped" }
  "ignored done"
end

def integer(&callback)
  callback&.call { 7 }
end

[false, true].each do |present|
  callback = present ? proc { |value, &execute| execute.call } : nil
  puts statement(&callback)
  p expression(&callback)
  unused = present ? proc { |value| "unused" } : nil
  puts ignored(&unused)
  number = present ? proc { |&execute| execute.call } : nil
  p integer(&number)
end
