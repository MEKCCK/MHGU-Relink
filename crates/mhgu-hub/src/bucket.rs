use std::sync::Mutex;
use std::time::{Duration, Instant};

/// Token bucket: `rate` tokens a second, at most `burst` saved up.
pub struct TokenBucket {
    inner: Mutex<State>,
}

struct State {
    rate: f64,
    burst: f64,
    tokens: f64,
    last: Instant,
}

impl TokenBucket {
    pub fn new(rate: f64, burst: f64) -> Self {
        TokenBucket {
            inner: Mutex::new(State {
                rate,
                burst,
                tokens: burst,
                last: Instant::now(),
            }),
        }
    }

    fn refill(st: &mut State, now: Instant) {
        st.tokens += now.duration_since(st.last).as_secs_f64() * st.rate;
        if st.tokens > st.burst {
            st.tokens = st.burst;
        }
        st.last = now;
    }

    /// Spends `n` tokens if there are that many.
    pub fn take(&self, n: f64) -> bool {
        let mut st = self.inner.lock().unwrap();
        Self::refill(&mut st, Instant::now());
        if st.tokens < n {
            return false;
        }
        st.tokens -= n;
        true
    }

    /// Spends `n` tokens and returns how long to wait before they are really there.
    pub fn wait(&self, n: f64) -> Duration {
        let mut st = self.inner.lock().unwrap();
        Self::refill(&mut st, Instant::now());
        st.tokens -= n;
        if st.tokens >= 0.0 {
            return Duration::ZERO;
        }
        let secs = -st.tokens / st.rate;
        if secs.is_finite() && secs > 0.0 {
            Duration::from_secs_f64(secs)
        } else {
            Duration::ZERO
        }
    }
}
