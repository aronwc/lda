rtm.em <-
function (documents, links, K, vocab, num.e.iterations, num.m.iterations, 
    alpha, eta, lambda = sum(sapply(links, length))/(length(links) * 
        (length(links) - 1)/2), initial.beta = rep(3, K), trace = 0L, 
    test.start = length(documents) + 1L) 
{
    estimate.params <- function(document.sums) {
        z.bar. <- t(document.sums + alpha)/colSums(document.sums + 
            alpha)
        first.index <- rep(1:length(links), times = sapply(links, 
            length))
        second.index <- unlist(links) + 1L
        pi.bar <- z.bar.[first.index, ] * z.bar.[second.index, 
            ]
        p <- colSums(pi.bar)
        p <- p/(p + lambda * alpha * alpha * length(links) * 
            (length(links) - 1)/2)
        log(p/(1 - p))
    }
    num.e.iterations <- rep(num.e.iterations, length.out = num.m.iterations)
    result <- rtm.collapsed.gibbs.sampler(documents, links, K, 
        vocab, num.e.iterations[1], alpha, eta, initial.beta, 
        trace, test.start)
    beta <- estimate.params(result$document_sums)
    for (ii in seq(length.out = num.m.iterations - 1)) {
        cat("M-step iteration ")
        print(ii + 1)
        result <- rtm.collapsed.gibbs.sampler(documents, links, 
            K, vocab, num.e.iterations[ii + 1], alpha, eta, beta, 
            trace, test.start)
        beta <- estimate.params(result$document_sums)
    }
    c(result, list(beta = beta))
}
