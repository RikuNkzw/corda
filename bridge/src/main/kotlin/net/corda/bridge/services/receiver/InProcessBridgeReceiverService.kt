package net.corda.bridge.services.receiver

import net.corda.bridge.services.api.*
import net.corda.bridge.services.util.ServiceStateCombiner
import net.corda.bridge.services.util.ServiceStateHelper
import net.corda.core.internal.readAll
import net.corda.core.utilities.contextLogger
import net.corda.nodeapi.internal.config.SSLConfiguration
import net.corda.nodeapi.internal.protonwrapper.messages.ReceivedMessage
import rx.Subscription

class InProcessBridgeReceiverService(val conf: BridgeConfiguration,
                                     val auditService: BridgeAuditService,
                                     haService: BridgeMasterService,
                                     val amqpListenerService: BridgeAMQPListenerService,
                                     val filterService: IncomingMessageFilterService,
                                     private val stateHelper: ServiceStateHelper = ServiceStateHelper(log)) : BridgeReceiverService, ServiceStateSupport by stateHelper {
    companion object {
        val log = contextLogger()
    }

    private val statusFollower: ServiceStateCombiner
    private var statusSubscriber: Subscription? = null
    private var receiveSubscriber: Subscription? = null
    private val sslConfiguration: SSLConfiguration

    init {
        statusFollower = ServiceStateCombiner(listOf(auditService, haService, amqpListenerService, filterService))
        sslConfiguration = conf.inboundConfig?.customSSLConfiguration ?: conf
    }

    override fun start() {
        statusSubscriber = statusFollower.activeChange.subscribe {
            if (it) {
                val keyStoreBytes = sslConfiguration.sslKeystore.readAll()
                val trustStoreBytes = sslConfiguration.trustStoreFile.readAll()
                amqpListenerService.provisionKeysAndActivate(keyStoreBytes,
                        sslConfiguration.keyStorePassword.toCharArray(),
                        sslConfiguration.keyStorePassword.toCharArray(),
                        trustStoreBytes,
                        sslConfiguration.trustStorePassword.toCharArray())
            }
            stateHelper.active = it
        }
        receiveSubscriber = amqpListenerService.onReceive.subscribe {
            processMessage(it)
        }
    }

    private fun processMessage(receivedMessage: ReceivedMessage) {
        filterService.sendMessageToLocalBroker(receivedMessage)
    }

    override fun stop() {
        stateHelper.active = false
        if (amqpListenerService.running) {
            amqpListenerService.wipeKeysAndDeactivate()
        }
        receiveSubscriber?.unsubscribe()
        receiveSubscriber = null
        statusSubscriber?.unsubscribe()
        statusSubscriber = null
    }
}