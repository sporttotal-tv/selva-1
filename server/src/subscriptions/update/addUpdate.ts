import SubscriptionManager from '../subsManager'
import sendUpdate from './sendUpdate'
import { Subscription } from '../'

var delayCount = 0

const sendUpdates = (subscriptionManager: SubscriptionManager) => {
  // console.log(
  //   'SEND UPDATES - handled events:',
  //   subscriptionManager.stagedForUpdates.size,
  //   subscriptionManager.incomingCount
  // )

  subscriptionManager.stagedForUpdates.forEach(subscription => {
    subscription.inProgress = false
    // console.log(
    //   'update subscription and clear inProgress',
    //   subscription.channel
    // )
    subscriptionManager.stagedForUpdates.delete(subscription)
    sendUpdate(subscriptionManager, subscription)
      .then(v => {
        // console.log('SEND UPDATE FOR', subscription.channel)
      })
      .catch(err => {
        console.log('WRONG ERROR IN SENDUPDATE')
      })
  })

  subscriptionManager.stagedInProgess = false
  subscriptionManager.incomingCount = 0
  subscriptionManager.isBusy = false

  if (subscriptionManager.memberMemCacheSize > 1e5) {
    console.log('memberMemCache is larger then 100k flush')
    subscriptionManager.memberMemCache = {}
    subscriptionManager.memberMemCacheSize = 0
  }
  delayCount = 0
}

// 3 per ms
const eventsPerMs = 3

const delay = (subscriptionManager, time = 1000, totalTime = 0) => {
  if (totalTime < 10e3) {
    const lastIncoming = subscriptionManager.incomingCount

    if (subscriptionManager.isBusy) {
      console.log('server is busy wait longer')
      time += 5e3
    }

    delayCount++
    console.log('delay #', delayCount, lastIncoming)
    subscriptionManager.stagedTimeout = setTimeout(() => {
      const incoming = subscriptionManager.incomingCount - lastIncoming
      if (incoming / time > eventsPerMs) {
        // too fast ait a bit longer
        // reset count
        // subscriptionManager.incomingCount = 0
        // increase time
        time = Math.round(time * 1.1)
        // delay again
        subscriptionManager.stagedTimeout = setTimeout(() => {
          delay(subscriptionManager, time, totalTime + time)
        }, time)
      } else {
        // do it
        sendUpdates(subscriptionManager)
      }
    }, time)
  } else {
    console.log(
      '10 seconds pass drain',
      totalTime,
      'incoming',
      subscriptionManager.incomingCount
    )
    // do it now
    sendUpdates(subscriptionManager)
  }
}

const addUpdate = (
  subscriptionManager: SubscriptionManager,
  subscription: Subscription
) => {
  if (subscription.inProgress) {
    if (!subscriptionManager.stagedInProgess) {
      console.error('CANNOT HAVE BATCH UPDATES IN PROGRESS + SUBS IN PROGRESS')
    }
  } else {
    subscriptionManager.stagedForUpdates.add(subscription)
    subscription.inProgress = true
    if (!subscriptionManager.stagedInProgess) {
      subscriptionManager.stagedInProgess = true
      subscriptionManager.stagedTimeout = setTimeout(() => {
        const { incomingCount } = subscriptionManager
        if (incomingCount < 1000 && !subscriptionManager.isBusy) {
          sendUpdates(subscriptionManager)
        } else {
          delay(subscriptionManager)
        }
      }, 10)
    }
  }
}

export default addUpdate
