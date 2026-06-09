# Backticks update $? from the command status.

`false`
puts($? == 0)
`true`
puts($? == 0)
`ruby -e "exit 42"`
puts($? == 42 * 256)
