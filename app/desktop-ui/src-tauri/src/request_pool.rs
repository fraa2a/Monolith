use std::{
    io,
    sync::{mpsc, Arc, Mutex},
    thread,
};

pub struct Pool<T: Send + 'static> {
    sender: Option<mpsc::SyncSender<T>>,
    workers: Vec<thread::JoinHandle<()>>,
}
impl<T: Send + 'static> Pool<T> {
    pub fn new(
        workers: usize,
        capacity: usize,
        process: impl Fn(T) + Send + Sync + 'static,
    ) -> io::Result<Self> {
        if workers == 0 || capacity == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "request pool requires workers and queue capacity",
            ));
        }
        let (sender, receiver) = mpsc::sync_channel(capacity);
        let receiver = Arc::new(Mutex::new(receiver));
        let process = Arc::new(process);
        let mut pool = Self {
            sender: Some(sender),
            workers: Vec::with_capacity(workers),
        };
        for index in 0..workers {
            let receiver = receiver.clone();
            let process = process.clone();
            pool.workers.push(
                thread::Builder::new()
                    .name(format!("media-{index}"))
                    .spawn(move || loop {
                        let job = receiver
                            .lock()
                            .unwrap_or_else(|err| err.into_inner())
                            .recv();
                        let Ok(job) = job else {
                            break;
                        };
                        if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| process(job)))
                            .is_err()
                        {
                            eprintln!("media request worker panicked");
                        }
                    })?,
            );
        }
        Ok(pool)
    }
    pub fn submit(&self, value: T) -> Result<(), T> {
        match self
            .sender
            .as_ref()
            .expect("request pool sender missing")
            .try_send(value)
        {
            Ok(()) => Ok(()),
            Err(mpsc::TrySendError::Full(value) | mpsc::TrySendError::Disconnected(value)) => {
                Err(value)
            }
        }
    }
}
impl<T: Send + 'static> Drop for Pool<T> {
    fn drop(&mut self) {
        self.sender.take();
        for worker in self.workers.drain(..) {
            let _ = worker.join();
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;
    #[test]
    fn workers_and_queue_have_fixed_capacity_and_resume_after_saturation() {
        let gate = Arc::new((Mutex::new(false), std::sync::Condvar::new()));
        let worker_gate = gate.clone();
        let (started_tx, started_rx) = mpsc::channel();
        let (done_tx, done_rx) = mpsc::channel();
        let pool = Pool::new(2, 2, move |job: u8| {
            started_tx.send(job).unwrap();
            let (lock, changed) = &*worker_gate;
            let mut released = lock.lock().unwrap();
            while !*released {
                released = changed.wait(released).unwrap();
            }
            done_tx.send(job).unwrap();
        })
        .unwrap();
        pool.submit(1).unwrap();
        pool.submit(2).unwrap();
        started_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        started_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        pool.submit(3).unwrap();
        pool.submit(4).unwrap();
        assert_eq!(pool.submit(5), Err(5));
        assert!(
            started_rx.try_recv().is_err(),
            "only two workers may run concurrently"
        );
        let (lock, changed) = &*gate;
        *lock.lock().unwrap() = true;
        changed.notify_all();
        for _ in 0..4 {
            done_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        }
        pool.submit(6).unwrap();
        assert_eq!(done_rx.recv_timeout(Duration::from_secs(2)).unwrap(), 6);
        drop(pool);
        assert!(started_rx.recv_timeout(Duration::from_secs(2)).is_ok());
    }
}
