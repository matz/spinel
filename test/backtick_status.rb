# Backticks update $? from the command's exit status.

`false`
puts($? == 0)
`true`
puts($? == 0)
