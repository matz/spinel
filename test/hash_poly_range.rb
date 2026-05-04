H = {
  success: 200..299,
  ok:      200,
}
puts H[:ok]

# multiple mixed values
STATUS = {
  info:    100..199,
  success: 200..299,
  count:   2,
}
puts STATUS[:count]

# range include? still works
puts((200..299).include?(250))
puts((200..299).include?(300))

# poly array with range
a = [200..299, 200]
puts a[1]
