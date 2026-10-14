import './assets/hakkas.css'
import './components/model'

import { createApp } from 'vue'
import App from './App.vue'
import {settleTheme} from './theme'

import 'material-icons/iconfont/material-icons.css';

settleTheme().catch(e => console.log("No theme list:", e));
createApp(App).mount('#app')
