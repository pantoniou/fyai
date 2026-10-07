# Cost estimate

A provider can report the cost of a call, as OpenRouter does. The others report
tokens only. For these, fyai prices the call from the catalogue and marks the
result as an estimate.

## The price of a call

`fyai_pricing_lookup()` finds the offering of the model of the call, on the
provider that served it, and reads `input`, `output`, `extra.cache_read` and
`extra.cache_write`. A missing cache price is the input price. A model with no
price has no estimate.

The cost is the sum of four parts, in USD per million tokens:

- the prompt tokens that were not cached, at the input price;
- the cached tokens, at the cache read price;
- the cache write tokens, at the cache write price; and
- the output tokens, with the reasoning tokens, at the output price.

`fyai_extract_usage()` stores `cost`, `est` and `model` on the usage of each
call. The model is the model of that call, so a conversation that changed model
is priced call by call. A reported cost is kept as it is and `est` is false.

A provider that does not report cached tokens gives an upper bound: the
estimate prices the whole prompt at the input price.

## The total

The status row and `stats` total the usage that the turns carry. A session that
starts from a stored conversation shows its cost at once: the counters are made
again when the head moves (`fyai_usage_sync()`). A total that includes an
estimate starts with `~`. `stats` gives the estimated part as
`cost_est`. A sub-agent counts its own calls.

## A change of model

A prompt cache belongs to one model. After a change, the first request sends
the whole conversation at the full input price, where the old model would
have read it from its cache. For an offering that caches on request only, the
price is the cache write price.

`/model NAME` says what that costs, and the status row shows
`cache miss ~+$X` until the next call ends. The estimate is the difference
between the two prices of the prompt tokens. The prompt size is the measured
size of the last call, with an estimate of what was added after it.

`stats` counts the changes of model in the stored conversation, as `switches`.
For each, `switch_cost` adds what the call read at full price and its model
would have charged at the cache price.
